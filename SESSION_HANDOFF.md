# PRADYOS — SESSION HANDOFF

> A fresh Claude Code session (possibly on a new account/machine) should read this
> file **in full** before touching anything. It captures the exact repo state and
> the non-negotiable contracts so the project continues with zero context loss.

---

## 0. RESUME INSTRUCTION (read this first, act in this order)

> **"Layer 5 is COMPLETE (5a–5f minus the deferred items below). Begin **NET-B —
> lwIP TCP/IP over the virtio-net driver**. Read SESSION_HANDOFF.md in full. Run
> `graph_session_primer()`. Run the full gate set and confirm green. Write the
> ADR/DDR before code. Do NOT restart earlier slices; do NOT start Layer 6
> (AETHER) until NET-B is gate-green."**

Concretely:
1. Read this whole file (esp. §0.1 — current state).
2. `graph_session_primer()` — or `node tools/graph_mcp/server.js primer`.
   (Graph node_modules already installed here; fresh clone: `cd tools/graph_mcp
   && npm ci && node server.js rebuild`.)
3. Run the full gate set (§6) **plus** `smoke-fpu smoke-init smoke-shell` and
   confirm green before editing.
4. Begin **NET-B (lwIP)** — port lwIP over the existing virtio-net driver (NET-A).
   ADR/DDR first. Gate: a loopback/UDP/TCP `smoke-net`-style test.

**5d/5e closed this session** (HEAD `9f310da`): per-thread FPU save (ADR-023 §D8),
pradyos-init PID 1 (ADR-023 §5d), and the **PRISM shell** (ADR-024) — interactive
ring-3 shell over the serial console, builtins help/echo/cat/run/ls/ps/exit, with
console RX (IRQ4 ring buffer) and **full-register fork** now in the kernel.

**Open follow-ups (deferred, see ADR-024 / build_status):**
- **FAT32 large-file read / `execve` of a large musl-C ELF corrupts** — PRISM is
  kernel-spawned from SFS instead; init-`execve` respawn waits on this fix.
- `ls`/`ps` are stubs (need `SYS_GETDENTS` / a process-table syscall); RX line
  discipline/echo; pipes/redirection/quoting/job-control/scripting.

### 0.1 CURRENT STATE (end of session 2026-06-27) + PROC-D resume plan

- **HEAD:** `9f310da`  (5d+5e; `main` == `dev/phase1`, pushed by refspec; this
  worktree branch is `claude/pedantic-shirley-a27bf3` — see §7). Gate set now also
  includes `smoke-fpu`, `smoke-init`, `smoke-shell` (all in CI).
- **Build distro:** **Ubuntu-24.04** (NOT 22.04 — 22.04 is gone; `sudo` needs a
  password now: the WSL password is the user's to supply).
- **Toolchain:** `llvm-objcopy` was missing on 24.04 → installed apt pkg
  **`llvm-18`**, symlinked **`/usr/local/bin/llvm-objcopy`** → `llvm-objcopy-18`
  (also repaired a stale apt index so `libpfm4` resolved). `clang-18`/`ld.lld-18`
  present; qemu/mkfs.fat/mcopy/mkfs.ext4/nasm/rustup present.
- **Stored remote PAT in `.git/config` is EXPIRED** — push with a fresh token
  inline in the URL (never write it to a tracked file).
- **Gates at HEAD `0cfd957`:** all 8 green — `toolchain-check, image, smoke,
  smoke-fs, smoke-fs-rw, smoke-fs-sfs-rw, smoke-fs-ext4, smoke-user`.

**Completed (PROC-D — musl libc — is COMPLETE):**
- **Layer 5b + IMP/PROC/NET series** were already complete on entry (the old
  handoff was stale — claimed HEAD `4608e9b`/"5b not started"). Reconciled.
- **PROC-D Step 1** (`f2bd207`): `SYS_SET_TLS`=27 (FS-base thread pointer,
  user-range-validated, restored on switch-in from `tcb.fs_base`, fork-inherited),
  `SYS_WRITEV`=28 (iovec gather-write via the validated copyin path; shared
  `fd_write_user`), `EXEC_MAX` 8 KiB→**256 KiB** (PMM-pool buffer). Probe
  `user/tlstest.asm` (`PRADYOS_TLS_OK WRITEV_OK`).
- **PROC-D Steps 2+3** (`8dd2162`, `0cfd957`): musl **v1.2.5** (`third_party/musl`,
  commit `0784374d`) builds to `build/musl/lib/{libc.a,crt1.o}` via `make musl` /
  `tools/build_musl.sh` + `third_party/musl-overlay/`. `user/cmusl.c` is the first
  ring-3 C program — links static against the subset with `user/user_c.ld`, runs
  from SFS, prints `PRADYOS_MUSL_OK v1.2.5 2026` via `printf`→`SYS_WRITEV`.
  `cpu_enable_sse()` enables x87/SSE for the varargs ABI. Design + the hard-won
  details are in **ADR-023** (§D1–D8).

**musl usage for 5d/5e (how to build a ring-3 C program):**
- Compile: `clang --target=x86_64-elf -ffreestanding -fno-pic -fno-pie
  -mcmodel=large -nostdinc -nostdlib -Wall -Wextra -Werror
  -Ibuild/musl/include -Ithird_party/musl/arch/x86_64
  -Ithird_party/musl/arch/generic -Ithird_party/musl/include -c foo.c -o foo.o`
  (**`-mcmodel=large` is mandatory** — the 0x8000000000 base exceeds 32-bit relocs).
- Link: `ld.lld -nostdlib -static -no-pie -T user/user_c.ld
  build/musl/lib/crt1.o foo.o build/musl/lib/libc.a -o foo.elf`.
- Embed like `cmusl` (incbin in `arch/x86_64/user_image.asm` →
  `user_boot_from_sfs` in `kernel/main.c`), or place on FAT/SFS for execve.
- If a libc call hits an undefined symbol at link, add its musl source file to the
  `SRCS` list in `tools/build_musl.sh` (the subset is link-resolved, not exhaustive).
- A new syscall musl needs that the NSI lacks: add it to the NSI (append-only) and
  remap it in `third_party/musl-overlay/syscall_overrides.h`.

**⚠️ BEFORE 5e (or any two concurrent ring-3 C/SSE processes):** add per-thread
`FXSAVE`/`FXRSTOR` to the context switch (512-byte 16-aligned area in the TCB,
saved/restored in `schedule()` like `fs_base`). Today FPU/XMM state is NOT saved
across switches — correct only while one thread uses the FPU at a time (**ADR-023
§D8**, binding trigger). 5d (single PID-1 process) is still safe.

**Next build order:** **5d pradyos-init (PID 1 + orphan reaper)** → 5e PRISM shell
→ NET-B lwIP → Layer 6 AETHER. ADR/DDR before each slice's code.

---

## 1. PROJECT IDENTITY

- **Name:** PRADYOS — Sovereign Edition (kernel: **NEXUS**).
- **Purpose / one-liner:** a from-scratch, bare-metal, AI-native operating system
  with an original x86_64 kernel; built in strict layer/slice order — *a slice
  ships when it is correct, not when it is fast.*
- **Active branch:** `dev/phase1` (fast-forwarded into `main` per slice).
- **Current HEAD:** `0cfd957` — "feat(PROC-D): musl printf gate — PROC-D
  complete". `main` == `dev/phase1`. (The rest of §2 below predates this session
  and is partially stale — §0.1 is authoritative for current state; Layer 5b +
  the IMP/PROC/NET series + **PROC-D (musl)** are all complete.)
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
