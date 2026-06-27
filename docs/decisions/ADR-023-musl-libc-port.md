# ADR-023: musl libc port (PROC-D) — minimal subset over the PRADYOS NSI

- **Status:** Accepted 2026-06-27 — design record for Layer 5 slice PROC-D (this *is* **DDR-PROC-D**)
- **Date:** 2026-06-27
- **Phase:** 5 (slice PROC-D — "5c musl" in the original roadmap)
- **Relation to prior ADRs:** builds on **ADR-022** (NSI v2 syscall surface,
  validated copy path, FD table) and **ADR-012** (SYSCALL/SYSRET mechanism); bound
  by **ADR-021** (W^X / NX / user-fault isolation, *binding security*). Nothing
  here may weaken ADR-021: the musl image is still one RX text segment plus
  R+NX rodata and RW+NX data/bss/heap/stack, exactly as the ELF loader enforces.

> **Why an ADR for "port a libc"?** Because a real libc is the first ring-3 code
> the kernel runs that it did **not** hand-write to the NSI. musl makes hard
> assumptions the kernel must satisfy *before* `main` — a thread pointer, a
> working `write`, an allocator backend — and it is statically linked from
> upstream source we do not control. The contract (which musl syscalls map to
> which NSI calls, what the kernel must add, what is shimmed in userspace, and
> what is explicitly out of scope) is recorded here, before code.

---

## Context

### Goal (PROC-D scope, fixed by the slice brief)
- Cross-compile **upstream musl** against the PRADYOS syscall ABI (NSI v2).
- Minimal subset first: `string.h`, `stdlib.h`, `stdio.h` (**write-only**, to the
  serial console on fd 1).
- A ring-3 **C** program using `printf` must compile against this musl and print
  to serial.
- Gate: `smoke-user` must cover the musl `printf` test.

### Version pin (do not float)
- **musl 1.2.5**, git tag `v1.2.5`,
  commit `0784374d561435f7c787a555aeab8ede699ed298`
  (`git.musl-libc.org/git/musl`). Pinned as a git submodule under
  `third_party/musl` (source only; the built `libc.a` and objects are
  git-ignored build artifacts, never committed — consistent with the repo's
  "no generated artifacts" rule). Reproduced by `git submodule update --init`.
  - Rationale for a submodule over a vendored copy: keeps the 1.2 MB musl tree
    out of our history, makes the exact upstream commit auditable, and lets the
    PRADYOS deltas live as a small overlay rather than a fork.

### The three things musl needs that the NSI does not yet provide
1. **A thread pointer (FS base).** On x86_64, musl dereferences the thread
   pointer (`%fs:0`) in `__init_tp` / `errno` / `__stdio_*` before `main`. With
   no FS base set, the *first* libc call faults. The NSI has no way to set it.
2. **A large-enough loader budget.** Every user-ELF path is hard-capped at
   **8 KiB** today (`EXEC_MAX 8192u` in `kernel/syscall/sys_exec.c:39`, the SFS
   bootstrap buffer, and the `≤ 8192` Makefile check). A statically-linked musl
   `printf` is far larger than 2 pages (`vfprintf` alone dwarfs it). No musl
   binary can load until this cap is raised.
3. **`writev`.** musl's `__stdio_write` issues `writev` with two iovecs (the
   buffered data + the caller's trailing chunk). The NSI has `SYS_WRITE` but no
   `writev`.

Everything else musl's minimal `printf`/`malloc` path touches maps cleanly, or
is shimmed in the userspace overlay (see the mapping table).

---

## Decision

### D1 — Reuse musl's stock x86_64 `syscall_arch.h`; override only the numbers
PRADYOS uses the **Linux x86_64 syscall register convention** (number in RAX;
args in RDI, RSI, RDX, R10, R8, R9; return in RAX; `syscall` instruction —
see `kernel/syscall/syscall.h`). This is *identical* to musl's stock
`arch/x86_64/syscall_arch.h`, so we keep it unchanged. Only the **syscall
numbers** diverge (NSI is a dense, append-only space; Linux numbers are
unrelated). We override them with a PRADYOS `bits/syscall.h` overlay rather than
patching musl source.

### D2 — Implement the port as a small overlay, not a fork
Overlay lives under `third_party/musl-pradyos/` (tracked) and is layered on the
pinned submodule at build time:
- `arch/x86_64/bits/syscall.h` — `__NR_*` → NSI numbers (table below). Calls the
  kernel *does not* have are given **negative sentinels**; the overlay's C shims
  intercept those `__NR_*` before they reach the `syscall` instruction.
- `pradyos_syscall_shim.c` — translates the handful of musl syscalls whose shape
  the NSI does not match 1:1 (see mapping). Keeps the kernel surface minimal.
- `crt/` — `_start` already exists in asm for the 5a hand-written programs; for
  C we use musl's own `crt1.o` startup (it calls `__libc_start_main`), so the
  loader must deliver a valid SysV `argc/argv/envp/auxv` (it already does, 5a).

### D3 — Kernel additions (exactly two; both root-cause, both reusable)
1. **`SYS_SET_TLS` (NSI 27)** — `set_tls(uintptr_t fs_base)`: writes the
   `IA32_FS_BASE` MSR (0xC0000100) for the current thread and stores it in the
   TCB so it is restored on context switch. This is the analog of Linux
   `arch_prctl(ARCH_SET_FS)`. Capability: none required (a thread setting its own
   FS base is not a privileged cross-process action; it cannot escape its AS).
   Validated: `fs_base` must be a canonical, user-range address or `-EINVAL`
   (never let ring 3 point FS at kernel space — defense in depth even though
   `%fs:` accesses are still subject to the page tables).
2. **`SYS_WRITEV` (NSI 28)** — `writev(fd, iov*, iovcnt)`: iterates the user
   `iovec` array (each base/len validated via the **existing `copyin` path**,
   never raw-dereferenced) and reuses the `sys_write` per-fd logic. Returns total
   bytes or `-EFAULT`/`-EBADF` exactly like `sys_write`. Needed by musl stdio and
   broadly useful.

NSI stays append-only; `SYS_IO_URING_ENTER` is 26, so 27/28 are the next free
slots. `SYS_SET_TLS` and `SYS_WRITEV` never move.

### D4 — Raise the user-ELF loader budget from 8 KiB to a PMM-pool buffer
The 2-page bootstrap buffer is replaced by a **PMM-pool-backed** read buffer
sized to the file, capped at **`EXEC_MAX = 256 KiB`** (64 pages). Rationale and
constraints:
- Per the kernel-lowmem rule, large buffers come from the **PMM pool, not BSS**
  (the low-mem image cap forbids a 256 KiB static array). The loader does
  `pmm_alloc` of `ceil(size/4096)` pages, reads the ELF into it, builds the
  image, then frees the buffer.
- `EXEC_MAX` (`sys_exec.c`), the in-kernel SFS bootstrap loader (`main.c`), and
  the Makefile size check are updated **together** to the same 256 KiB ceiling.
  The hand-written 5a/5b asm programs stay tiny; only the cap changes, so they
  are unaffected.
- 256 KiB is comfortably above a minimal-subset static musl `printf` binary and
  still far below any memory-pressure concern on the q35 target. If a future
  program needs more, raise the constant in one place (documented here).
- **W^X is untouched:** the loader still maps `PT_LOAD` segments per `p_flags`
  (RX text, R+NX rodata, RW+NX data). A bigger *input buffer* changes nothing
  about the *mapped* permissions. ADR-021 regression `wxviol` must still pass.

### D5 — Syscall → NSI mapping (minimal `printf` + `malloc` subset)

| musl `__NR_*` | Used by | NSI mapping | Handling |
|---|---|---|---|
| `write` | `__stdio_write` fallback, `string`/`stdlib` error paths | `SYS_WRITE` (6) | direct |
| `writev` | `__stdio_write` (2-iovec flush) | **`SYS_WRITEV` (28)** | direct (new) |
| `arch_prctl`(SET_FS) | `__init_tp` startup | **`SYS_SET_TLS` (27)** | shim: drop the `code` arg, pass `addr` |
| `set_thread_area` | (not used on x86_64) | — | n/a |
| `exit` / `exit_group` | `_Exit`, `__libc_exit_fini` | `SYS_EXIT` (4) | shim: both → `SYS_EXIT` |
| `mmap` | `malloc` (mallocng) | `SYS_MMAP` (12, 4-arg) | shim: anon-only (`fd=-1`); drop `fd`/`off`, pass `addr,len,prot,flags` |
| `munmap` | `malloc` (free of large) | `SYS_MUNMAP` (13) | direct |
| `madvise` / `mremap` | mallocng (optional) | — | shim → `-ENOSYS` (mallocng tolerates) |
| `ioctl`(TCGETS) | `stdout` isatty probe | — | shim → `-ENOTTY` (musl falls back to full buffering; we set `_IONBF` on stderr anyway) |
| `getpid` | misc | `SYS_GETPID` (2) | direct |

Notes:
- The 6-arg ABI widening that ADR-022 deferred is **still deferred**: the only
  6-arg caller in scope is `mmap`, and the overlay shims it to the existing 4-arg
  `SYS_MMAP` because the subset only ever requests `MAP_ANONYMOUS` (where `fd`
  and `offset` are ignored by definition). If a future slice needs `MAP_FILE`
  from libc, *that* slice widens the ABI — not this one.
- `read` (stdin) is out of scope for write-only stdio; musl's `stdin` exists but
  is never flushed/read in the gate program.

### D6 — Build integration
- `make musl` (new phony): `git submodule update --init`, then build the subset
  with the existing cross toolchain
  (`clang --target=x86_64-elf -ffreestanding -fno-pic …`, `ld.lld`) producing
  `build/musl/libc.a`. musl's own `configure` is bypassed for the subset; we
  compile the specific source files the subset needs (string/*, stdio write
  path, stdlib malloc + exit, the overlay shims) with the PRADYOS overlay on the
  include path ahead of musl's `arch/x86_64`.
- New gate program `user/cmusl.c` (C, not asm): calls `printf("…")` and
  `return 0`. Linked with musl `crt1.o` + `build/musl/libc.a` + `user/user.ld`
  at `0x8000000000`, written to SFS, loaded back, run in ring 3. Its serial
  banner is grepped by **`smoke-user`** (extend the existing gate; do not add a
  parallel one unless the matrix demands it).
- `-Werror` stays green for **our** overlay and the gate program. Upstream musl
  is third-party source built with musl's own warning posture (it is not part of
  our `-Werror` surface); our overlay `.c`/`.h` are.

### D7 — Writable user data segment + startup/TLS (added after inspecting musl)
Discovered while wiring step 2: two assumptions in D2/D6 were incomplete.

1. **User programs need a RW+NX data/bss segment.** `user/user.ld` emits a single
   R+X segment (text+rodata) and discards the rest — correct for the 5a/5b asm
   programs (no mutable globals), but musl has writable global state (the `stdout`
   `FILE`, the `libc` struct, malloc/`ofl` state, the static `builtin_tls` block).
   New script **`user/user_c.ld`** emits three W^X-clean PT_LOADs the existing ELF
   loader already maps per `p_flags`:
   - `text`  → `PF_R|PF_X` : `.text`
   - `rodata`→ `PF_R`      : `.rodata*` (R + NX, per ADR-021)
   - `data`  → `PF_R|PF_W` : `.data*` + `.bss*` (RW + NX)
   No RWX segment, no W+X segment — ADR-021 is preserved. The asm programs keep
   using `user/user.ld` unchanged.
2. **TLS goes through musl's own startup, not a hand-rolled TCB.** musl's
   `__set_thread_area` (x86_64 asm) issues `arch_prctl(ARCH_SET_FS, TP)`. We
   **override** it with a one-line overlay shim that calls `SYS_SET_TLS(TP)`
   directly (so `SET_TLS` keeps its `(fs_base)` contract — no arch_prctl
   emulation in the kernel). The gate program is linked with musl's `crt1.o` →
   `__libc_start_main` → `__init_libc`/`__init_tls`, which lays out the pthread
   TCB (errno, locale, the static `builtin_tls`) correctly. Startup syscalls that
   the NSI does not implement (`set_tid_address`, `ioctl(TCGETS)`,
   `rt_sigprocmask`) map to unregistered NSI slots; `syscall_dispatch` returns
   `-ENOSYS`, which musl tolerates for these (no panic — verified in `syscall.c`).

Build mechanics consequence for D6: the overlay supplies a generated
`bits/syscall.h` (musl's x86_64 numbers with `write→6, writev→28, mmap→12,
munmap→13, exit/exit_group→4` remapped to the NSI; all other `__NR_*` keep their
upstream values and resolve to `-ENOSYS` if ever issued). The hand-compiled file
set is whatever `__libc_start_main` + `printf` + `malloc` + the string subset
reference; it is resolved by linking and is enumerated in the Makefile's
`musl` target.

### D8 — Enable x87/SSE for ring-3; per-thread FPU save deferred (found in step 3)
The x86_64 SysV ABI uses XMM registers — e.g. `printf`'s variadic prologue saves
`%xmm0..7` — so any C program `#UD`s without `CR4.OSFXSR`. `cpu_enable_sse()`
(`kernel/arch/x86_64/cpu_mitigations.c`, called once in `kmain`) sets
`CR0.MP`/clears `CR0.EM` and sets `CR4.OSFXSR|OSXMMEXCPT`. The kernel is built
`-mgeneral-regs-only` and never touches the FPU/XMM.
- **RESOLVED (5d):** the context switch now does **eager per-thread FPU
  save/restore**. The TCB carries a 512-byte 16-aligned `fpu_state` (the kmalloc
  pool is ≥16-aligned and the field attribute keeps it on a 16-byte boundary, as
  FXSAVE/FXRSTOR require). `schedule()` does `fxsave(prev->fpu_state)` then
  `fxrstor(next->fpu_state)` immediately before `context_switch` — the single
  switch path (per `graph_callchain`), beside the CR3/`fs_base` restores; nothing
  between there and the switch touches the FPU (`-mgeneral-regs-only`). New
  threads (and `idle`) copy a clean template captured once in `sched_init`
  (`fninit` + MXCSR=0x1F80 + `fxsave`) — a *zeroed* FXSAVE area is **not** clean
  (MXCSR=0 unmasks all SSE exceptions). Forked children inherit the parent's
  `fpu_state`. **Eager, not lazy** (no `CR0.TS`/`#NM` dance): simple, correct, and
  free of the LazyFP (CVE-2018-3665) register-leak class. Lazy/XSAVE-optimized
  save is a Layer-7 perf option, not a correctness need.
  Gate **`smoke-fpu`**: two concurrent ring-3 processes each pin their pid into
  XMM0 and verify it survives ~30M preemption-interleaved iterations; both print
  `PRADYOS_FPU_OK` (gate also forbids `PRADYOS_FPU_FAIL`). Verified OK=2/FAIL=0.

---

## Consequences

### Positive
- First real C-on-PRADYOS: unblocks PROC-D's successors — **5d `pradyos-init`
  (PID 1)** and **5e PRISM shell** — which are ordinary C programs.
- Two genuinely reusable kernel primitives (`SYS_SET_TLS`, `SYS_WRITEV`) and a
  loader that can run real-sized binaries.
- Faithful upstream musl (pinned, auditable) rather than a hand-rolled libc.

### Negative / accepted cost
- A submodule + overlay is more build machinery than the asm programs.
- 256 KiB transient PMM allocation per exec (freed immediately after image
  build) — acceptable on the target; documented.

### Explicitly deferred (out of scope for PROC-D)
- **Dynamic linking** (`ld-pradyos.so`) — static only, as for all ring-3 code so far.
- **`math.h`**, the full `stdio` surface (read side, `fmemopen`, wide chars,
  locale), `pthread`/threads, signals-in-libc, `setjmp` beyond what `printf`
  pulls.
- **6-arg syscall ABI widening** / `MAP_FILE` from libc (ADR-022 deferral stands).
- **`writev` blocking / partial-write retry semantics** beyond the `sys_write`
  baseline (console never blocks; pipe path keeps its non-blocking baseline).

### Invariants this slice must not break
- ADR-021 W^X/NX/user-fault isolation — unchanged; `wxviol` regression stays green.
- NSI append-only — 27/28 appended, nothing renumbered.
- `-Werror` (clang + nasm) on our code; `docs/build_status.md` updated in the
  **same commit** as the code it describes.

---

## Build order after PROC-D
`5d pradyos-init (PID 1, orphan reaper)` → `5e PRISM shell` → `NET-B lwIP` →
`Layer 6 AETHER agent runtime` (only after 5e + NET-B). Mandatory; no skipping.
