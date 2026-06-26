# PRADYOS — SESSION HANDOFF

> A fresh Claude Code session (possibly on a new account/machine) should read this
> file **in full** before touching anything. It captures the exact repo state and
> the non-negotiable contracts so the project continues with zero context loss.

---

## 0. RESUME INSTRUCTION (read this first, act in this order)

> **"You are continuing PROC-D Step 2 build-out. Read SESSION_HANDOFF.md in full.
> Run `graph_session_primer()`. Run all 8 gates and confirm green. Then build the
> PROC-D Step 2 items in order (§0.1), each compiling clean before the next."**

Concretely:
1. Read this whole file (esp. §0.1 — current PROC-D state + the exact build plan).
2. `graph_session_primer()` (MCP) — or `node tools/graph_mcp/server.js primer`.
   (Graph node_modules already installed in this worktree; if a fresh clone,
   `cd tools/graph_mcp && npm ci && node server.js rebuild`.)
3. Run the full gate set (see §6) and confirm **all 8 green** before editing.
4. Continue **PROC-D Step 2 build-out** (§0.1). Do NOT restart earlier slices —
   Layer 5b and PROC-D Step 1 are DONE and committed.

### 0.1 CURRENT STATE (end of session 2026-06-27) + PROC-D resume plan

- **HEAD:** `0688afc`  (`main` == `dev/phase1`; this worktree branch is
  `claude/pedantic-shirley-a27bf3` — push by refspec to both, see §7).
- **Build distro:** **Ubuntu-24.04** (NOT 22.04 — 22.04 is gone; `sudo` needs a
  password now: the WSL password is the user's to supply).
- **Toolchain fix applied this session:** `llvm-objcopy` was missing on 24.04.
  Installed apt pkg **`llvm-18`** and symlinked **`/usr/local/bin/llvm-objcopy`**
  → `llvm-objcopy-18`. (Also repaired a stale apt index so `libpfm4` resolved.)
  `clang-18`/`ld.lld-18` were already present; qemu/mkfs.fat/mcopy/mkfs.ext4/
  nasm/rustup all present.
- **Gates at HEAD `0688afc`:** all 8 green — `toolchain-check, image, smoke,
  smoke-fs, smoke-fs-rw, smoke-fs-sfs-rw, smoke-fs-ext4, smoke-user`.

**Completed this session:**
- **Layer 5b + IMP/PROC/NET series** were already complete on entry (the old
  handoff was stale — it claimed HEAD `4608e9b`/"5b not started"). Reconciled.
- **PROC-D Step 1 DONE** (`f2bd207`): `SYS_SET_TLS`=27 (FS-base thread pointer,
  user-range-validated, restored on switch-in from `tcb.fs_base`, inherited on
  fork), `SYS_WRITEV`=28 (iovec gather-write via the validated copyin path;
  shared `fd_write_user` helper), `EXEC_MAX` 8 KiB→**256 KiB** (PMM-pool buffer
  in `sys_exec` + the SFS bootstrap loader; Makefile cap follows). Ring-3 probe
  `user/tlstest.asm` gated in `smoke-user` (`PRADYOS_TLS_OK WRITEV_OK`).
- **PROC-D Step 2 PARTIAL** (`0688afc`): musl **v1.2.5** pinned at
  `third_party/musl/` (commit `0784374d`); **ADR-023 §D7** records the design
  (writable user data segment, musl startup/TLS path, overlay shims). The musl
  build is **not wired into the kernel build yet**, so all gates stay green.

**PROC-D Step 2 build-out — do these in order, each clean before the next:**
- **2a. `user/user_c.ld`** — 3-segment W^X linker script: `text` PF_R|PF_X,
  `rodata` PF_R (R+NX), `data` PF_R|PF_W (`.data*`+`.bss*`, RW+NX). Based on
  `user/user.ld` but adds the writable segment (musl has writable globals). Verify
  it links a dummy C object with no warnings. (ADR-023 §D7.1.)
- **2b. `third_party/musl/overlay/bits/syscall.h`** — remap `write→6, writev→28,
  mmap→12, munmap→13, exit→4, brk→9 (if used)`; all other `__NR_*` pass through
  from musl's native x86_64 table (unimplemented ones resolve to `-ENOSYS`).
- **2c. `third_party/musl/overlay/__set_thread_area.s`** — one-line shim issuing
  `SYS_SET_TLS` (27) with the TP in RDI instead of `arch_prctl` (musl's stock
  version does `mov %rdi,%rsi; mov $0x1002,%edi; mov $158,%eax; syscall`). Also
  needs the 6-arg `mmap`→4-arg `SYS_MMAP` (anon) shim (ADR-023 §D5/§D7.2).
- **2d. Makefile `musl` target** — enumerate the ~15-20 sources
  `__libc_start_main`+`printf`+`malloc`+string subset pull in (resolve by
  linking): `crt/crt1.o`, `src/env/__libc_start_main.c` + `__init_tls.c` +
  `__init_libc.c` + `__libc_start_main`'s deps, `src/stdio/printf.c` +
  `vfprintf.c` + `fwrite.c` + `__stdio_write.c` + the FILE plumbing,
  `src/malloc/…`, `src/string/*` subset, our overlay shims. Output
  `third_party/musl/lib/libc.a` + `crt/crt1.o`. Build BEFORE the user stage;
  zero warnings. Overlay include path goes AHEAD of `arch/x86_64`.
- **2e. Gate:** `make image` PASS + `make smoke-user` PASS (no regressions).
  Commit; print:
  `Phase PROC-D step 2 — gate report` / `<commit> | image PASS | smoke-user PASS | next: step 3 cmusl.c`

**PROC-D Step 3 (same session if Step 2 gates green):**
- **3a.** `user/cmusl.c` — ring-3 C calling `printf("PRADYOS_MUSL_OK\n")`, `return 0`.
- **3b.** Link vs `third_party/musl/lib/libc.a` + `crt1.o` using `user/user_c.ld`.
- **3c.** Embed in SFS (like `systest`), execve from `smoke-user`, grep `PRADYOS_MUSL_OK`.
- **3d.** All 8 gates green; `docs/build_status.md` marks **PROC-D COMPLETE**
  (same commit as the code).
- **3e.** Final report:
  `Phase PROC-D — COMPLETE` / `<commit> | all 8 gates PASS | next: 5d pradyos-init`

**Known risk for next session:** Step 2d/3 is the first C/TLS bring-up on this
kernel — expect QEMU debug cycles around `__init_tls`/auxv/stdio. `syscall_dispatch`
already returns `-ENOSYS` for unregistered numbers (no panic), so musl's
`set_tid_address`/`ioctl`/`rt_sigprocmask` degrade gracefully (verified).

After Step 3, the build order continues: **5d pradyos-init (PID 1 + orphan
reaper) → 5e PRISM shell → NET-B lwIP → Layer 6 AETHER**.

---

## 1. PROJECT IDENTITY

- **Name:** PRADYOS — Sovereign Edition (kernel: **NEXUS**).
- **Purpose / one-liner:** a from-scratch, bare-metal, AI-native operating system
  with an original x86_64 kernel; built in strict layer/slice order — *a slice
  ships when it is correct, not when it is fast.*
- **Active branch:** `dev/phase1` (fast-forwarded into `main` per slice).
- **Current HEAD:** `0688afc` — "build(PROC-D): pin musl v1.2.5 submodule +
  ADR-023 design revision (D7)". `main` == `dev/phase1`. (The rest of §2 below
  predates this session and is partially stale — §0.1 is authoritative for the
  current PROC-D state; Layer 5b + the IMP/PROC/NET series are all complete.)
- **Repo path (this machine):**
  - Windows: `C:\Users\prady\Documents\Claude\Projects\Prady4OS`
  - WSL: `/mnt/c/Users/prady/Documents/Claude/Projects/Prady4OS`
- **Remote:** `https://github.com/prady4the4bady/Prady4OS.git` (a PAT lives only in
  the local `.git/config`; **never** write it to a tracked file. On a new account,
  configure your own credentials; do not reuse a leaked token.)

---

## 2. CURRENT STATE — LAYER BY LAYER

### ✅ Complete & CI-verified
- **L1 Boot** — MBR two-stage loader (`boot/mbr`, `boot/stage2`) → long mode → ring-0 C. Gate: `smoke`.
- **L2 NEXUS kernel core** — GDT/IDT + CPU-exception panic path; 8259 PIC + 8254 PIT @100 Hz; buddy PMM (ADR-003); SLAB heap; higher-half VMM (ADR-007); round-robin preemptive scheduler (ADR-008); NCS capability system (ADR-009); NIA IPC sync/async/broadcast (ADR-010/011); syscalls + ring 3 via SYSCALL/SYSRET + TSS (ADR-012); preemption-safe shared state (ADR-016). Gate: `smoke`.
- **L3 Drivers** — ACPI + PCIe ECAM (ADR-013); reusable virtio 1.0 transport + generic block layer + interrupt-driven virtio-blk (ADR-014); CMOS RTC (ADR-020). Gates: covered by FS gates.
- **L4 VFS + filesystems** — mount table + per-mount context (ADR-015/017); **FAT32 read-write** + VFAT long-name read + RTC timestamps (ADR-015/020); **SFS** inode CoW B+tree, file extents, journal + atomic tx + crash replay, snapshots, per-extent LZ4, 4 KiB tags (ADR-018); **ext4 read-only** (ADR-019). Gates: `smoke-fs`, `smoke-fs-rw`, `smoke-fs-sfs-rw`, `smoke-fs-ext4`.
- **L5a Userspace — static ELF loader + W^X (ADR-021)** — per-process address spaces + per-process CR3; `EFER.NXE` (CPUID-gated); ELF64 loader maps `PT_LOAD` with `p_flags`→W^X perms; 8 MiB RW+NX stack + guard page; SysV `argc/argv/envp/auxv`; ring-3 entry; user fault → clean kill. Test ELF written to SFS and **loaded back from SFS**. Gate: `smoke-user`.

CI status as of this handoff: **all 8 gates green** (`toolchain-check`, `image`,
`smoke`, `smoke-fs`, `smoke-fs-rw`, `smoke-fs-sfs-rw`, `smoke-fs-ext4`,
`smoke-user`) **plus** the `code-graph` job — run
[27874439210](https://github.com/prady4the4bady/Prady4OS/actions/runs/27874439210).
**Re-verify the current run yourself before trusting this** (see §9).

### 🟡 In progress / partial
- **L5 Userspace** — only slice **5a** done. Slices 5b (syscalls), 5c (musl),
  5d (pradyos-init PID 1), 5e (PRISM shell), 5f (prad) are **pending**. Next = 5b.

### ⏸ Deferred (tracked, with governing ADR)
- **Kernel-self W^X** (kernel text RX / kernel data NX) — the bootloader maps the
  kernel image RWX; only *user* W^X is enforced today. Governed by **ADR-021**;
  build before running untrusted code in kernel space (kernel-hardening pass).
- **COW fork** — interim will be copy-all-pages (5b); COW later. Record in **DDR-5b**.
- **Process reaping / AS teardown on exit** — `vmm_destroy_address_space` exists and
  is used on load-failure paths, but an exited user process leaks its AS + kstack
  until a reaper. Build with the PID-1 orphan reaper (slice **5d**).
- **Dynamic linking** (`ld-pradyos.so`) — static ELF only for now.
- **APIC / SMP** — legacy PIC+PIT only (**ADR-006**).
- **3-lane NAS scheduler** — round-robin placeholder (**ADR-008**).
- **PMM variable-weight / predictive** — buddy interim (**ADR-003**, OPEN).
- **ext4 write**, **SFS free-space tree / snapshot GC** — read/feature scope set by **ADR-018/019**.
- Full deferred list with "build-before" triggers: `docs/build_status.md` → DEFERRED.

---

## 3. ARCHITECTURE CONTRACTS THAT MUST NEVER BE BROKEN

These are **binding**. ADR-021 is a binding security ADR and may only be changed by
a new superseding ADR — never quietly amended.

1. **W^X invariant (ADR-021).** No page is ever simultaneously writable and
   executable, in any address space, anywhere in Layer 5.
   - text/`PF_X` → present, USER, **RX** (no W, no NX).
   - rodata/`PF_R` → present, USER, **R + NX**.
   - data/BSS/`PF_W`, heap, user stack → present, USER, **RW + NX**.
   - guard page → **not present** (PTE = 0), placed immediately below the stack.
   - A segment that is both `PF_W` and `PF_X` is **rejected** (`ELF_E_WX`), never mapped RWX.
   - Enforced in `kernel/exec/elf.c` (flag derivation) + `kernel/mm/vmm.c`
     (`map_core` writes PTE bit 63 = NX). **What breaks it:** mapping any user page
     with both `VMM_RW` and without `VMM_NX`; reusing the deleted RWX `user_demo`;
     dropping the W+X rejection. Negative regression `user/wxviol.asm` must keep
     producing `#PF err=0x7 → clean kill`.
2. **NX gating.** `EFER.NXE` is enabled **only if** `CPUID.8000_0001h:EDX[20]`
   reports NX (`g_nx_ok` in `kernel/mm/vmm.c`). When `g_nx_ok` is false, `map_core`
   must **not** set PTE bit 63 (setting it with NXE clear faults). Never force NXE
   unconditionally.
3. **User-fault isolation (ADR-021).** A fault from **CPL 3** (`isr_dispatch`,
   `kernel/idt.c`) must become a clean process kill via `sched_exit` with a
   `[trap] user …` line — **never** a kernel panic. CPL-0 faults still panic.
4. **Root-cause-only fixes.** No patchwork, no warning suppression, no masking
   symptoms. If a design issue appears, write/adjust an ADR or DDR first.
5. **`-Werror` must stay green at all times** — clang **and** nasm
   (`NASM_WERROR := -Werror`). Any warning is a build failure.
6. **Repo stays clean & organized.** No `TODO`/`FIXME`, no dead code/refs, no new
   flat files in `kernel/` root (use subsystem subdirs). `docs/build_status.md` is
   updated in the **same commit** as the code it describes; keep
   `docs/platform_profiles.md` accurate too.

---

## 4. PHASE C — LAYER 5b EXACT PLAN

Build order is **mandatory**; do not skip/reorder without a DDR. Each slice:
runs `graph_session_primer()` + `graph_deps()` before editing; writes its
ADR/DDR **before** code; adds `smoke-user` coverage; keeps `-Werror` clean;
updates `docs/build_status.md` in the same commit; and must pass its gate before
the next slice starts.

1. **DDR-5b (design, written FIRST)** — the syscall-expansion plan; the validated
   user-pointer contract (`copyin` / `copyout` / `copyinstr`, **EFAULT, never
   panic**, all user pointers validated against the process AS); and the explicit
   **fork = copy-all-pages now, COW deferred** decision.
2. **`copyin` / `copyout` primitives** + a bad-pointer / guard-page test
   (→ `EFAULT`, kernel survives). Comes after DDR-5b.
3. **`read` / `write`** (using the validated copy path).
4. **`open` / `close`.**
5. **`lseek` / `fstat` / `getcwd`.**
6. **`mmap` baseline** (MAP_ANON; W^X-respecting flags).
7. **`execve`** (replace the AS; reuse the ELF loader).
8. **`wait4`.**
9. **Signal groundwork** (+ the fork decision record if true COW is deferred).

**Gate report format (print after each slice gate, exactly):**
```
Phase 5b — gate report
<commit> | <gate> | PASS/FAIL | next: <next slice>
```

---

## 5. GRAPH TOOLING (use it FIRST, every session)

- **Location:** `tools/graph_mcp/` — pure JS/WASM code knowledge graph (MCP + CLI).
  Full usage: `tools/graph_mcp/CLAUDE_GRAPH_USAGE.md`.
- **MCP server:** registered in `/.mcp.json` (`pradyos-graph`). The client prompts
  once to trust it. Manual start: `node tools/graph_mcp/server.js mcp`.
- **Fresh clone / first run:** `bash tools/graph_mcp/setup_graph.sh`
  (ensures Node ≥18, `npm ci`, builds + validates the graph). On Windows:
  `cd tools/graph_mcp && npm ci && node server.js rebuild`.
- **MANDATORY workflow:**
  - **`graph_session_primer()` before opening ANY source file** (orientation).
  - **`graph_deps("<file>")` before editing any file** (includes / included-by / users).
  - **`graph_blast_radius("<file>")` before any refactor or signature change.**
  - `graph_query("…")` / `graph_files("<subsystem>")` to locate code instead of blind reads.
  - `graph_callchain("<fn>")` before changing a function's contract.
  - **`graph_rebuild()` after structural changes** (new/renamed/moved files, new functions).
- **CLI fallback** (if MCP not connected):
  `node tools/graph_mcp/server.js {primer|query <t>|files <ss>|deps <f>|callchain <fn>|blast <f>|rebuild|selftest}`.

---

## 6. CI DETAILS

- **Workflow:** `.github/workflows/ci.yml`. Runs on push to `main` / `dev/**` and PRs.
- **Job `build-and-boot`** — must stay green:
  `toolchain-check` → `image` (`-Werror`) → `smoke` → `smoke-fs` → `smoke-fs-rw`
  → `smoke-fs-sfs-rw` → `smoke-fs-ext4` → `smoke-user`.
- **Job `code-graph`** (isolated, non-destructive) — `npm ci` then
  `node server.js selftest`: builds the graph from source and validates the 7
  query tools (writes only the git-ignored `.graph/` DB; cannot make kernel gates flaky).
- **Local full gate set (run before committing):**
  ```
  wsl -d Ubuntu-24.04 -- bash -lc 'cd /mnt/c/Users/prady/Documents/Claude/Projects/Prady4OS \
    && source "$HOME/.cargo/env" && make toolchain-check && make image && make smoke \
    && make smoke-fs && make smoke-fs-rw && make smoke-fs-sfs-rw && make smoke-fs-ext4 && make smoke-user'
  ```
- **Verify CI from the host:** `gh run list/watch --repo prady4the4bady/Prady4OS`
  (use the Windows `gh`; it is not on the WSL PATH).

---

## 7. TOOLCHAIN NOTES

- **Builds run in WSL**, not native Windows:
  `wsl -d Ubuntu-24.04 -- bash -lc 'cd <repo> && source "$HOME/.cargo/env" && make <target>'`.
  Cross toolchain (ADR-001): clang + `ld.lld` + `llvm-objcopy` + nasm; Rust nightly
  `x86_64-unknown-none`; QEMU q35.
- **WSL has no Linux `node`** — only the Windows `node`/`npm` leak in via PATH
  interop. So the graph tooling uses a **pure JS/WASM stack** (`sql.js` +
  `web-tree-sitter@0.22.6` + `tree-sitter-wasms`): one `node_modules` works on the
  Windows host, WSL, **and** Linux CI — no `node-gyp`, no native prebuilds, no CI
  flakiness. `setup_graph.sh` installs a Linux Node (≥18) in WSL when missing.
- **Shell-quoting gotcha (cost real time):** inline shell variables and loops
  inside `wsl … bash -lc '…'` from the agent's Bash tool **expand to empty**
  (MSYS2/Git-Bash argv mangling) — e.g. `L=foo; … "$L"` → empty, `for g in …; do
  make $g; done` → no target. `$HOME`, `$(…)`, and `<<'EOF'` heredocs survive. Use
  **literal commands / fixed paths / heredocs**, or put loops in a Makefile target.
- **WSL git identity** is per-repo (set this session): user.name
  `pradyun kumar sinha`, user.email `pradyun4kumar4sinha4@icloud.com`. A new
  account should set its own.
- **NASM** is parsed by a focused extractor (not Tree-sitter) — ASM grammars lack
  reliable prebuilt WASM; this is intentional and documented.

---

## 8. FILE OWNERSHIP MAP

| Path | Layer / role | Read ADR before editing |
|------|--------------|--------------------------|
| `boot/mbr/`, `boot/stage2/` | L1 bootloader | ADR-005 |
| `arch/x86_64/*.asm` | L2 asm (boot, cpu, isr, context, syscall_entry, usermode) | ADR-005/012 |
| `arch/x86_64/user_image.asm` | L5 — embeds `build/hello.elf` + `build/wxviol.elf` | ADR-021 |
| `kernel/mm/pmm.*`, `kheap.*` | L2 memory | ADR-003 |
| `kernel/mm/vmm.*` | L2 VMM + **per-process AS / NX** | **ADR-007 + ADR-021 (binding)** |
| `kernel/proc/sched.*`, `tss.*` | L2 scheduler + per-process CR3 | ADR-008 (+ ADR-021 for cr3) |
| `kernel/cap.*` | L2 capabilities | ADR-009 |
| `kernel/ipc/*` | L2 IPC + broadcast | ADR-010/011 |
| `kernel/syscall/*` | L2 syscalls (5b extends this) | ADR-012 |
| `kernel/idt.c` | L2 exception dispatch + **user-fault kill path** | ADR-021 |
| `kernel/exec/elf.*` | **L5 ELF loader (binding security)** | **ADR-021** |
| `kernel/acpi/*`, `kernel/drivers/*` | L3 drivers | ADR-013/014/020 |
| `kernel/fs/vfs/*` | L4 VFS + mounts | ADR-015/017 |
| `kernel/fs/fat32/*` | L4 FAT32 RW + LFN | ADR-015/020 |
| `kernel/fs/sfs/*` | L4 SFS engine | **ADR-018** |
| `kernel/fs/ext4/*` | L4 ext4 read-only | ADR-019 |
| `kernel/main.c` | boot orchestration + in-kernel self-tests | — |
| `user/hello.asm`, `user/wxviol.asm`, `user/user.ld` | L5 ring-3 test programs | ADR-021 |
| `tools/graph_mcp/**` | tooling (not a kernel layer) | `CLAUDE_GRAPH_USAGE.md` |
| `docs/decisions/ADR-*.md` | architecture decisions | — |
| `docs/build_status.md`, `docs/platform_profiles.md` | status (update with code) | — |

**Generated — never commit (git-ignored):** `build/`, `tools/graph_mcp/.graph/`,
`tools/graph_mcp/node_modules/`, `*.o`/`*.elf`/`*.bin`/`*.img`, `target/`.
The graph DB and `node_modules` are reproduced via `npm ci` + `rebuild`.

---

## 9. WHAT NOT TO DO

- ❌ **Do not weaken W^X or NX to make a test pass.** Fix the test or the real bug.
- ❌ **Do not commit generated artifacts** (`build/`, `.graph/`, `node_modules/`, images, objects).
- ❌ **Do not patchwork.** Root-cause fixes only; ADR/DDR before governed code.
- ❌ **Do not claim CI is green without verifying the current run** (`gh run watch`).
- ❌ **Do not proceed past a failing gate** — stop and fix the root cause first.
- ❌ **Do not skip `graph_session_primer()` at session start**, or `graph_deps()`
  before editing, or `graph_blast_radius()` before a refactor.
- ❌ **Do not start slice N+1** until slice N boots clean and passes its gate.
- ❌ **Do not invent ISA/register details** — cite Intel/AMD SDM or say "I don't know".
- ❌ **Do not write the PAT/secrets into any tracked file.**

---

*Generated at the end of the session that completed Layer 5a (ADR-021) and added
the `tools/graph_mcp` code knowledge graph. HEAD `4608e9b`. Next: Phase C, Slice 1
— DDR-5b.*
