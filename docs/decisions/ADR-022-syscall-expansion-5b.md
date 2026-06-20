# ADR-022: Layer 5b syscall expansion (NSI v2) — validated copy path, FD table, process control

- **Status:** Accepted 2026-06-21 — design record for Layer 5b (this *is* **DDR-5b**)
- **Date:** 2026-06-21
- **Phase:** 5 (slice 5b)
- **Relation to prior ADRs:** extends **ADR-012** (the SYSCALL/SYSRET mechanism +
  the 4 bootstrap calls) and is bound by **ADR-021** (W^X / NX / user-fault
  isolation, *binding security*). Nothing here may weaken ADR-021; where the two
  interact (mmap permissions, copy-path fault handling) ADR-021 governs.

> **Why an ADR for a "just add syscalls" slice?** Because the calls are easy but
> the contracts are not: the kernel is about to start *dereferencing pointers it
> received from ring 3*. Getting the validated-copy contract, the FD/capability
> bridge, and the fork model wrong would either open a kernel-compromise hole or
> violate the binding W^X policy. Those decisions are recorded here, before code,
> per CLAUDE.md ("ADR/DDR before the code it governs").

---

## Context

### What userspace needs
Slices 5c–5f (musl libc, `pradyos-init` PID 1, the PRISM shell, the `prad`
package manager) are ordinary statically-linked ring-3 programs. To run them the
kernel must expose a POSIX-shaped syscall surface large enough that a static musl
build and a shell can: read/write files and the console, open/close/seek/stat
files, get anonymous memory, fork, exec another program, and reap children. That
is the whole job of 5b — *the substrate the rest of Layer 5 is written against.*

### What the kernel actually has after Layer 5a (verified this session)
- **Mechanism (ADR-012).** `SYSCALL`/`SYSRET` armed (`EFER.SCE`, `STAR`, `LSTAR`,
  `SFMASK=0x200` clears `IF` so syscalls never nest). ABI: number in `RAX`; args
  in `RDI, RSI, RDX, R10` (`R8/R9` are *saved* by the trampoline but not yet
  forwarded); return in `RAX`; `RCX/R11` clobbered.
  `arch/x86_64/syscall_entry.asm` switches to the thread's kernel stack and calls
  `syscall_dispatch`.
- **Dispatch (`kernel/syscall/syscall.c`).** `static syscall_fn table[16]`;
  `syscall_register(num, fn)`; `syscall_dispatch(num, a1, a2, a3, a4)` bounds-checks
  `num` and returns `-1` for an unknown/empty slot. `syscall_fn` is
  `long(*)(long,long,long,long)` — **only 4 args reach C today.**
- **Calls today:** `SYS_PUTC=1` (cap-gated console write), `SYS_GETPID=2`,
  `SYS_YIELD=3`, `SYS_EXIT=4` (calls `sched_exit`, never returns).
- **Address spaces (ADR-021/-007).** Per-process CR3; `vmm_new_address_space()`,
  `vmm_map_in(pml4_phys, …)`, `vmm_destroy_address_space()`; user range = PML4
  slot 1 = `[512 GiB, 1 TiB)`; PTE flags incl. `VMM_USER`, `VMM_RW`, `VMM_NX`;
  `EFER.NXE` CPUID-gated (`vmm_nx_enabled()`).
- **Fault path (ADR-021, `kernel/idt.c`).** A fault from **CPL 3**
  (`(r->cs & 3) == 3`) → `[trap] user …` → `sched_exit()`. A fault from **CPL 0
  panics.** *This is the central problem 5b must solve:* during a syscall the CPU
  is at **CPL 0**, so a naive `*user_ptr` against a bad pointer would `#PF` at
  CPL 0 → **kernel panic**. The copy contract below exists precisely to make that
  impossible.
- **VFS (`kernel/fs/vfs/vfs.h`).** Capability-gated:
  `vfs_open(cap, mnt, path, *out)`, `vfs_read(cap, *f, off, buf, len)`,
  `vfs_write(cap, *f, off, buf, len)`, `vfs_create`, `vfs_unlink`, `vfs_readdir`,
  txn ops. Files are `struct vfs_file` (size + FS-private cookie + owning mount).
  **There is no fd abstraction yet.**
- **ELF loader (`kernel/exec/elf.c`).** `elf_load(image, len, name, **out)` builds
  a fresh AS + ring-3 thread with W^X enforced. `execve` reuses this.
- **TCB (`kernel/proc/sched.h`).** `cr3`, `pid`, `is_user`, `caps`,
  `fs_write_budget`, `user_rip/stack/arg`. **No fd table, no parent pid, no exit
  status, no children list, no zombie state** — fork/wait4 add these.
- **Teardown gap.** `vmm_destroy_address_space` exists but an *exited* user
  process leaks its AS + kernel stack until a reaper (PID-1 reaper, slice 5d).

---

## Decision 1 — Validated user-pointer contract (the heart of 5b)

Every byte that crosses the ring boundary goes through three primitives. **The
kernel never dereferences a raw user pointer anywhere else.** This is a hard
rule, equal in standing to W^X: a code review that finds a bare `*user_ptr` /
`memcpy` from a user address in a syscall handler is a defect, not a style nit.

```c
/* kernel/mm/uaccess.{c,h} (new). Return 0 on success, -EFAULT on any bad access.
 * They NEVER fault at CPL 0 and NEVER panic on a user-supplied pointer. */
int copyin   (void *dst_kernel, const void *src_user, uint64_t n);
int copyout  (void *dst_user,   const void *src_kernel, uint64_t n);
int copyinstr(char *dst_kernel, const void *src_user, uint64_t max, uint64_t *out_len);
```

### Semantics
- **`copyin`** — copy `n` bytes from user `src` to kernel `dst`. `-EFAULT` if any
  byte of `[src, src+n)` is not a present, user-accessible page in the *calling
  process's* address space.
- **`copyout`** — copy `n` bytes from kernel `src` to user `dst`. `-EFAULT` if any
  byte of `[dst, dst+n)` is not present **and writable** (`VMM_RW`) and user in
  the calling AS. Writing a read-only user page (e.g. text/rodata) is `-EFAULT`,
  never an honored write — this is also how W^X is upheld on the copy path.
- **`copyinstr`** — copy a NUL-terminated string, at most `max` bytes incl. the
  terminator; `*out_len` = bytes copied. `-EFAULT` on a bad page; `-ENAMETOOLONG`
  if no NUL within `max`. Used for path arguments (`open`, `execve`).

### Mechanism — page-walk pre-validation (chosen)
Before touching any byte, walk the calling process's page tables and verify, for
**every** page the range spans:
1. the virtual address lies in the user range `[USER_BASE, USER_TOP)` (PML4
   slot 1) — a pointer into kernel/identity space is rejected outright, closing
   the confused-deputy hole where a user asks the kernel to read its own memory;
2. the PTE is `PRESENT | USER`;
3. for `copyout`, the PTE is additionally `RW`.

Only if the whole range validates do we `memcpy`. A new VMM helper does the walk:

```c
/* kernel/mm/vmm.h (new). 1 if every page in [virt, virt+len) is present, USER,
 * inside the user range, and (need_write ? RW : any); else 0. No allocation,
 * no faults. Walks the ACTIVE address space (the caller's, during its syscall). */
int vmm_user_range_ok(uint64_t virt, uint64_t len, int need_write);
```

### Why pre-validation is race-free here (and when to revisit)
A syscall runs with `IF=0` (`SFMASK`) on a **single CPU** with a
**non-preemptible kernel** — nothing can unmap or re-permission the validated
pages between the check and the copy. So the classic TOCTOU window does not
exist today. **This assumption is recorded as a precondition:** when SMP (APIC,
deferred) or a preemptible kernel arrives, `vmm_user_range_ok` + copy is no
longer atomic and the design must move to the *fault-fixup* approach (see
Alternatives). Until then, pre-validation is correct and far simpler.

### Hard guarantees
- **No `#PF` at CPL 0 from a user pointer, ever.** Bad pointer ⇒ `-EFAULT`
  returned to userspace; the kernel keeps running. (A genuine *kernel* bug still
  panics, as it must — CPL-0 panic semantics from ADR-021 are unchanged.)
- The copy primitives are the **only** place user memory is read/written by the
  kernel; all new syscalls below are built on top of them.

---

## Decision 2 — Syscall table & ABI (NSI v2)

### Dispatch & registration (unchanged shape, larger)
Keep the `syscall_register(num, fn)` + `table[]` model from ADR-012. Changes:
- **`MAX_SYSCALLS` 16 → 64** (headroom for the clusters below + growth).
- **Numbering:** `1..4` stay exactly as ADR-012 defined them (`putc/getpid/yield/
  exit`) so the 5a binary and the `smoke-user` regression keep working unchanged.
  New calls take fresh numbers in a stable, append-only block. PradyOS uses **its
  own number space, not Linux's** — musl is ported in 5c with a PradyOS arch
  layer that maps libc calls to these numbers (a normal new-OS musl port), so we
  are not bound to Linux ABI numbers and keep the table dense and auditable.
- **Unknown/empty slot** returns `-ENOSYS` (was `-1`); see errno below.

### ABI argument widening (required by `mmap`)
Today only `a1..a4` reach C. `mmap` needs up to 6 args. Decision:
- Widen `syscall_fn` and `syscall_dispatch` to **6 args** (`a1..a6`).
- `arch/x86_64/syscall_entry.asm` already preserves `R10/R8/R9`; extend the
  marshal so the SysV C call is `syscall_dispatch(num, a1, a2, a3, a4, a5)` —
  that is **6 integer registers** (`RDI..R9`), the SysV maximum. A **7th** value
  (`num`+`a6`) is passed on the kernel stack. For the 5b baseline this is only
  exercised by `mmap`'s 6th arg (`offset`), which **must be 0** for `MAP_ANON`,
  so the register path covers every real 5b call.
- This is a pure superset; the 4-arg calls are unaffected.

*(Cited: System V AMD64 ABI — first six integer/pointer args in
`RDI, RSI, RDX, RCX, R8, R9`; further args on the stack.)*

> **Implementation note (slice 6).** The widening is **deferred**: every 5b
> syscall fits in ≤4 args, including the `mmap` MAP_ANON baseline (which needs
> only addr/len/prot/flags — fd/offset are ignored for anonymous maps). The
> 4-arg dispatch therefore stands for all of 5b; the 6-arg marshal lands when a
> syscall (e.g. file-backed mmap with a real offset) actually needs a 5th/6th
> argument.

### Return / error convention
Handlers return a `long`: `>= 0` success (or count), **negative `errno`** on
failure (`-EFAULT`, `-EBADF`, `-ENOENT`, `-EINVAL`, `-ENOSYS`, `-ENAMETOOLONG`,
`-EMFILE`, `-ENOMEM`, `-ECHILD`, …). A new `kernel/syscall/errno.h` defines the
numeric values musl's arch port will mirror.

### Capability ↔ FD bridge (preserve NCS under a POSIX face)
POSIX file descriptors do not carry capabilities, but NCS gating (ADR-009) must
not be bypassed. Decision:
- A process is granted its capability set at spawn/exec (console cap, plus
  `CAP_FS_READ`/`CAP_FS_WRITE` for its mount(s)) — the same mechanism as 5a's
  console cap, generalized.
- **`open` checks the process FS capability** and, on success, records the
  authorized handle in the fd table entry. Subsequent `read`/`write`/`lseek`/
  `fstat`/`close` operate on the **fd**, and the kernel uses the stored cap
  internally — so NCS is still enforced on every op, but userspace sees plain
  fds. Raw cap handles are never passed across the syscall boundary by file I/O.

### Per-process FD table
Add to the process (TCB for now; a dedicated PCB later) a small **fixed fd table**
(32 entries). Each entry: `{ in_use, struct vfs_file, uint64_t offset, int mnt,
cap_t cap, int flags }`. fds **0/1/2** are pre-bound to the console (stdin/stdout/
stderr) at process creation, backed by the console capability so `write(1, …)`
works from libc with no special-casing. `open` returns the lowest free fd or
`-EMFILE`.

---

## Decision 3 — The 9 syscall clusters (build order = handoff §4)

Built **in this order**; each cluster is one slice, writes its `smoke-user`
coverage **before** it is considered done, keeps `-Werror` green, updates
`docs/build_status.md` in the same commit, and must pass its gate before the next
cluster starts. "Gate requirement" = the sentinel line(s) the in-kernel/user test
must emit (matched by `tools/qemu_runner/boot_test.sh` via `EXTRA_SENTINEL`).

| # | Cluster (slice) | Calls | `smoke-user` gate requirement |
|---|-----------------|-------|-------------------------------|
| 1 | **Copy primitives** (5b-2) | `copyin`, `copyout`, `copyinstr` + `vmm_user_range_ok` | A ring-3 probe passes a valid buffer (copy OK) **and** a guard-page / kernel-space / RO pointer → handler returns `-EFAULT`; **kernel survives** (no panic). Sentinels: `[uaccess] copy ok`, `[uaccess] EFAULT on bad ptr`. |
| 2 | **File I/O core** (5b-3) | `read`, `write` | Ring-3 `write(1,"…")` reaches the console via the fd/cap bridge; `read`/`write` of a file on SFS round-trips byte-exact through `copyin`/`copyout`. Sentinels: `[sys] write ok`, `[sys] read roundtrip ok`. |
| 3 | **Open / close + fd table** (5b-4) | `open`, `close` | `open` an existing SFS path → fd ≥ 3; `open` a missing path → `-ENOENT`; `close` frees the slot; exhausting the table → `-EMFILE`. Sentinels: `[sys] open ok`, `[sys] open ENOENT`, `[sys] close ok`. |
| 4 | **Positioning & metadata** (5b-5) | `lseek`, `fstat`, `getcwd` | `lseek` then `read` reads from the new offset; `fstat` reports the file size (via `copyout` of a `struct stat`); `getcwd` returns `/`. Sentinels: `[sys] lseek ok`, `[sys] fstat size ok`, `[sys] getcwd ok`. |
| 5 | **Memory mapping** (5b-6) | `mmap` (MAP_ANON), `munmap`, `brk`/`sbrk` | `mmap(NULL, len, PROT_READ\|PROT_WRITE, MAP_ANON\|MAP_PRIVATE, -1, 0)` returns a usable RW+NX region in the user range; writing+reading it works; `PROT_WRITE\|PROT_EXEC` is **rejected** (`-EINVAL`) — W^X (ADR-021) holds; `munmap` unmaps and a later access faults → clean kill. Sentinels: `[sys] mmap rw ok`, `[sys] mmap WX rejected`, `[sys] munmap ok`. |
| 6 | **Process creation** (5b-7) | `fork` (copy-all-pages — Decision 4) | `fork` returns child pid to the parent and `0` in the child; both run; child writes a distinct line; parent observes the child pid. Sentinels: `[sys] fork parent pid=…`, `[sys] fork child running`. |
| 7 | **Program replacement** (5b-8) | `execve` | A forked child `execve`s a second embedded ELF (path on SFS); the new image's `main` runs in the replaced AS with W^X enforced (reuses `elf_load`). Sentinel: `[sys] execve -> HELLO FROM EXEC`. |
| 8 | **Process reaping** (5b-9) | `wait4` (+ exit-status plumbing, zombie state) | Parent `fork`s a child that `exit(7)`s; `wait4` returns the child pid and decodes status 7; `wait4` with no children → `-ECHILD`. Sentinels: `[sys] wait4 reaped pid=… status=7`, `[sys] wait4 ECHILD`. |
| 9 | **Signal groundwork** (5b-10) | `kill`, `sigaction`, `sigprocmask`, `rt_sigreturn` **scaffolding** | The table accepts the calls and stores per-process disposition/mask; **synchronous default-terminate** for a fatal signal is wired (delivery on kernel→user return); custom handler *invocation* and the sigreturn trampoline are stubbed to `-ENOSYS` where not yet real, never faking success. Sentinels: `[sys] signal disposition stored`, `[sys] default-term on SIGKILL`. |

**Gate report after each slice (exactly):**
```
Phase 5b — gate report
<commit> | <gate> | PASS/FAIL | next: <next slice>
```

---

## Decision 4 — fork = copy-all-pages now; COW deferred to a future ADR

`fork` in 5b **eagerly copies every private user page** of the parent into fresh
frames for the child (kernel PML4 entries stay shared; only the user range is
duplicated). The child gets a new CR3, a duplicated fd table (same underlying
open files, independent offsets per POSIX), `parent_pid` set, and a cleared
return value (`0` in child, child pid in parent).

- **Why now:** correctness-first (CLAUDE.md: *a slice ships when it is correct,
  not when it is fast*). Copy-all-pages needs no PTE refcounting, no
  write-fault-driven copy, no shared-frame accounting — it is obviously correct
  and easy to test. init + a shell + small tools have tiny address spaces, so the
  memory/time cost is irrelevant at this stage.
- **COW is explicitly deferred** and may **only** be introduced by a **future
  superseding/companion ADR** (call it the "COW fork ADR"), never bolted on
  silently. That ADR will own: per-frame refcounts, marking shared pages
  read-only in both AS, a write `#PF` handler that copies-on-write and restores
  `RW` (interacting carefully with ADR-021 — the copied page must come back as
  `RW+NX`, never `RWX`), and frame-free accounting. **This ADR forbids** any
  partial/ad-hoc COW landing under 5b.
- **Dependency:** fork multiplies the existing teardown gap — more AS + kstacks
  leak on exit until the **PID-1 reaper (slice 5d)** lands. `wait4` (cluster 8)
  reaps the *zombie/exit-status* bookkeeping; the *physical* AS/kstack reclamation
  is wired with the reaper in 5d (tracked in `build_status.md` DEFERRED).

---

## Decision 5 — mmap baseline scope (and deferrals)

**In 5b (`mmap`/`munmap` baseline):**
- `MAP_ANONYMOUS | MAP_PRIVATE` only; `fd` must be `-1`, `offset` must be `0`.
- `addr == NULL`: the kernel picks a page-aligned region in the user range (a
  simple bump within slot 1, below the stack/guard). `addr != NULL` (a hint) is
  honored only if page-aligned, in-range, and free; otherwise `-EINVAL` (no
  silent relocation in the baseline).
- `prot`: `PROT_READ` and/or `PROT_WRITE` → pages mapped **`USER | RW(if W) | NX`**.
  **`PROT_EXEC` together with `PROT_WRITE` is rejected (`-EINVAL`)** — the same
  W^X rejection ADR-021 applies to ELF segments. (Executable anonymous mappings
  in general are out of scope for 5b; JIT, if ever, is a separate `CAP_JIT` layer
  per ADR-021.)
- **Eager allocation** for the baseline (frames mapped at `mmap` time) — simple
  and race-free with the copy contract. `len` rounded up to a page; `munmap`
  unmaps the range and reclaims frames.
- `brk`/`sbrk` over a per-process heap region (RW+NX) for musl's allocator.

**Deferred (need their own decision before building):** demand/lazy paging,
file-backed mmap (`MAP_SHARED`, fd-backed), `MAP_FIXED` overwrite semantics,
`mremap`, `msync`, `mprotect` (beyond what W^X allows), huge pages.

---

## Explicitly NOT in 5b

Recorded so scope cannot creep:
- **Dynamic linking** (`ld-pradyos.so`, `PT_INTERP`, shared objects) — static
  ELF only (ADR-021 stands); musl is linked **statically** in 5c.
- **Signals beyond groundwork** — no full custom-handler invocation, no real
  `rt_sigreturn` trampoline, no realtime queues, no `sigaltstack`. Only
  disposition/mask storage + synchronous default-terminate (cluster 9).
- **`futex`** — and therefore no userspace threads/locks that need it.
- **COW fork** (Decision 4) — future ADR only.
- **Threads / `clone`** — one thread per process in 5b.
- **File-backed / shared `mmap`, demand paging** (Decision 5).
- **Networking syscalls, `ptrace`, `select`/`poll`/`epoll`, `pipe`/`dup`
  beyond the fd-table primitives, TLS beyond what static musl needs.**
- **ext4 *write*** (ADR-019 keeps ext4 read-only) — file-write syscalls target
  FAT32/SFS mounts.

---

## Testing contract (binding for the slice)

**A cluster is not "done" until it has a `smoke-user` pattern proving it.** No
syscall is committed without a corresponding sentinel in the test program /
in-kernel self-test that `smoke-user` matches. Concretely:
- Each cluster adds the sentinel line(s) listed in its Decision-3 row;
  `smoke-user` must observe **all** of them plus the existing
  `NEXUS KERNEL OK` + 5a lines.
- **Negative tests are mandatory where a failure path exists** — at minimum the
  `copyin/copyout` `-EFAULT` case (cluster 1) and the `mmap` W^X rejection
  (cluster 5), each asserting the **kernel survives** (no panic, no weakened
  W^X).
- The full gate set (`toolchain-check`, `image` `-Werror`, `smoke`, `smoke-fs`,
  `smoke-fs-rw`, `smoke-fs-sfs-rw`, `smoke-fs-ext4`, `smoke-user`) must stay green
  on every slice commit; CI re-verifies (`gh run watch`).
- `node tools/graph_mcp/server.js rebuild` after any structural change (new
  files: `uaccess.{c,h}`, `errno.h`, fd-table additions) so the graph stays
  accurate.

---

## Alternatives considered

- **Fault-fixup copy (Linux-style exception table) instead of pre-validation.**
  Tag the faulting copy instructions; on a `#PF` at one of them, the handler
  jumps to a fixup that returns `-EFAULT` instead of panicking. *Pro:* atomic
  (no TOCTOU), the only correct option under SMP/preemptible kernels. *Con:*
  needs an `__ex_table`, fault-handler surgery in `kernel/idt.c`, and per-copy
  asm landing pads — more machinery than a single-CPU non-preemptible kernel
  needs. **Chosen:** pre-validation now; the fault-fixup design is the recorded
  upgrade path, triggered by SMP or a preemptible kernel.
- **Linux syscall numbers (binary musl/Linux ABI compat).** *Pro:* an unmodified
  Linux musl could run. *Con:* commits us to Linux's sprawling, sparse number
  space and ABI quirks forever, for an OS that ports its own static musl anyway.
  **Chosen:** own dense number space + a thin musl arch port in 5c.
- **COW fork from the start.** *Pro:* fast, memory-frugal fork. *Con:* refcounts +
  write-fault copy + W^X-correct restoration are exactly the kind of subtle
  coupling that wants its own ADR and its own tests; doing it under 5b risks a
  silent W^X regression. **Chosen:** copy-all-pages now, COW by future ADR.
- **Pass cap handles on every file syscall (no fd table).** *Pro:* maximal NCS
  explicitness. *Con:* not POSIX — musl/shell expect integer fds; leaks cap
  handles into userspace ABI. **Chosen:** cap checked at `open`, stored in the fd
  entry, enforced internally per op.

---

## Consequences

**Easier:** musl, init, and the shell (5c–5f) have a real target ABI; the kernel
can read/write user memory safely; fork/exec/wait give the shell process control.

**Harder / riskier (tracked):**
- The kernel now trusts pointers from ring 3 — mitigated entirely by Decision 1;
  the "no bare user deref" rule must be enforced in review.
- fork worsens the **AS/kstack leak on exit** until the PID-1 reaper (5d) —
  tracked in `build_status.md` DEFERRED; acceptable for short-lived 5b tests.
- The **pre-validation race-freedom depends on single-CPU + non-preemptible
  kernel** — recorded as a precondition; **must** switch to fault-fixup before
  SMP/preemptible-kernel work (UNCERTAIN until that hardware path exists).
- The 6-arg ABI widening touches `syscall_entry.asm` + `syscall_dispatch`;
  verified by every existing call continuing to pass `smoke-user`.

**Must be verified later (UNCERTAIN):** `getcwd` is trivially `/` until a real
cwd/namespace exists; `fstat` reports only the fields SFS/FAT32/ext4 actually
have; signal *delivery* semantics are revisited when real handlers land
(post-5b). These are called out so they are not mistaken for complete.
