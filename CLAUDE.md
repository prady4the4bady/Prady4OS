# PRADYOS — Claude Code session rules (CLAUDE.md)

PRADYOS Sovereign Edition: a from-scratch, bare-metal, AI-native OS for x86_64
(reference ISA). Work proceeds in strict layer/slice order; a slice ships when it
is correct, not when it is fast. **This file governs every session. Read it in
full before touching anything.**

---

## 1. Code graph FIRST — mandatory every session

A persistent code knowledge graph is available via the **`pradyos-graph`** MCP
server (registered in `.mcp.json`). Full usage:
`tools/graph_mcp/CLAUDE_GRAPH_USAGE.md`.

- **Start every session by calling `graph_session_primer()`** before opening any
  source file.
- **Orient via the graph, not blind reads:** `graph_query()` / `graph_files()`.
- **Call `graph_deps(file)` before editing a file.**
- **Call `graph_blast_radius(file)` before any refactor or signature change.**
- **Use `graph_callchain(fn)` before changing a function's contract.**
- **Call `graph_rebuild()` after structural changes** (new/renamed/moved files,
  new functions).

If the MCP server is not connected, the same queries work from the shell:
`node tools/graph_mcp/server.js {primer|query|files|deps|callchain|blast|rebuild}`.
First-time/fresh clone: `bash tools/graph_mcp/setup_graph.sh`.

---

## 2. Build, test, commit

- Build/test run in **WSL** (`wsl -d Ubuntu-24.04`). Source `$HOME/.cargo/env`
  before `make`. (`sudo` needs a password.)
- **Every gate must pass before commit** — except the gate that is the explicit
  target of the current active task (§5b below).
- Commit to **`dev/phase1`**, then fast-forward **`main`** per slice; push both.
  `main` always passes CI.
- Gate logs go under `build/gatelogs/`, **never** `/tmp` — WSL wipes `/tmp`.

---

## 3. Non-negotiables

- **Zero warnings** — `-Werror` for clang AND nasm. Root-cause fixes only;
  no suppression.
- **No new flat files in `kernel/` root** — use the subsystem subdirectories.
- **No `TODO`/`FIXME` placeholders, no dead code/refs** in committed code.
- **`docs/build_status.md` updated in the same commit as the code it describes.**
  Keep `docs/platform_profiles.md` accurate too.
- **ADR/DDR before the code it governs.** Binding ADRs may only be superseded by
  a new ADR, never quietly amended.
- Do not start slice N+1 until slice N boots clean and passes its gate.
- Never invent ISA/register details — cite Intel/AMD SDM or say "I don't know".
- **`kputhex` already emits its own `0x` prefix** (console.h:11). Never add a
  literal `0x` before a `kputhex` call.
- **`main` promotion rule:** three CI greens on the **same** tip before
  fast-forwarding `main`. Do not promote on two greens on different tips.

---

## 4. Orientation

- Status & component tracker: `docs/build_status.md`
- Layers, ISAs, branch strategy: `docs/platform_profiles.md`
- Decisions: `docs/decisions/ADR-*.md`
- Session state: `SESSION_HANDOFF.md` (repo root, NOT docs/)
- Feature state: `docs/AETHER_MASTER_FEATURES.md` (Sections A–H)
- Full backlog: `docs/BUILD_TRACKER.md`

---

## 5. Autonomous operation — READ THIS EVERY SESSION

This section is the governing authority for pacing and stopping. It supersedes
any conflicting instruction in `SESSION_HANDOFF.md` or any prior session note.

### 5a. Resume protocol

1. Read `SESSION_HANDOFF.md` in full.
2. Read `docs/build_status.md` to confirm the current tip SHA and gate count.
3. Run `graph_session_primer()`.
4. **Do NOT run the full gate suite before starting work.** Gates are run after
   the fix, not before.
5. Identify `CURRENT_ACTIVE_TASK` from `SESSION_HANDOFF.md` and start it.

### 5b. The active-task gate exception

If the current task is fixing a specific failing gate, that gate being red does
NOT block starting the task. Do not waste context verifying a known-broken gate
is still broken. Run the gate after implementing the fix.

### 5c. Context-limit protocol

- If context < 85%: **do NOT stop**. Keep working.
- If context ≥ 85%:
  1. Finish the current atomic operation (one function, one file — not a whole
     task).
  2. Build and run the gate for what was just written.
  3. Commit with an honest message (pass or fail, state which).
  4. Append a checkpoint block to `SESSION_HANDOFF.md`.
  5. Push both the work commit and the `SESSION_HANDOFF.md` commit.
  6. Stop — the next session resumes from the checkpoint.
- **Never stop mid-task saying "context limit" if context < 85%.** Check the
  actual percentage first.

### 5d. Autonomous task loop — KEEP GOING UNTIL EVERYTHING IS BUILT

Work through the backlog in §6 below **without waiting for human confirmation
between tasks.** The only valid stop conditions are:

1. Context window genuinely ≥ 85%.
2. A gate fails that is NOT the active repair target AND no fix is obvious.
3. A build error that requires an architectural decision not covered by existing
   ADRs.
4. A CI run is in flight and §6.1's local-QEMU rule applies.

**For everything else: keep going.** Do not stop at the end of a task and ask
what to do next. Read §6, pick the next unbuilt item, and start it.

---

## 6. The complete build backlog — work through this in order

The goal is to build **everything** in this list. Nothing here is optional except
the two pre-approved exceptions noted. Work items are ordered by dependency;
do not start an item whose prerequisites are not yet CI-green.

### 6.0 Mandatory pre-conditions (enforce every session)

- **§6.0-A — CI-in-flight rule:** before running QEMU locally, confirm no CI run
  is in flight on `dev/phase1`. Check with `gh run list --branch dev/phase1
  --limit 5`. If a run is in flight, wait for it rather than risk
  contention-induced false results.
- **§6.0-B — Instrument-first rule for active intermittents:** items 47 and 48
  (§6.1) must not be "fixed" on an unconfirmed hypothesis. Read the CI instrument
  output first; the fix follows from the evidence, not from guessing.
- **§6.0-C — One DDR per root cause:** do not conflate two independent failures
  into one fix. Items 47 and 48 are confirmed independent.
- **§6.0-D — Fix on confirmed, not on hypothesis:** §6 forbids shipping a
  virtio-blk driver change for the completion-loss hypothesis (DDR-775/776) until
  `reason=checksum-mismatch` appears in a CI failure with DDR-886's probes. Until
  then only diagnosability improvements are permitted.

### 6.1 Active intermittents — fix these before promotion

Both must be fixed and three CI greens earned on one tip before `main` moves.

**Item 47 — stalled `g_ticks` under `-smp 4` (~20% CI hit rate)**

The `[boot-load]` / `[boot-stamp]` instrument is live in the tree (from `e49a23f`
/ ADR-037). CI confirmed it works: a recent run printed
`[boot-load] PRISM.ELF t=3029 → [boot-stamp] B proofs-begin t=3093`.

When the next CI run fails:
- If `[boot-load]` / `[boot-stamp]` output is present: read the tick gap between
  `t=T1` and the stall point.
  - Gap ≈ 0 or no `[boot-stamp]` lines after `B proofs-begin` → `g_ticks` was
    not advancing → investigate LAPIC timer on APs, BSP-only `g_ticks` increment
    in `timer_tick`, and whether an AP timer race can starve the BSP tick path.
  - Gap large, ticks advancing → scheduler starvation — the proof loop never
    resumed from `yield()`. Investigate percpu scheduler state.
- If `[boot-load]` / `[boot-stamp]` output is absent: the item-47 failure
  occurred before `PRISM.ELF` was loaded. Read where the boot stalled and move
  the instrument earlier.
- Write the DDR before any fix code. Do not guess a fix without the captured
  failing run.

**Item 48 — virtio-blk probe false-failure under `-smp 4`**

DDR-886 (commit `2dc1bad`) fixed diagnosability. Both blk probes now drain
workers before verdict and print `done=<hex>` plus `reason=checksum-mismatch` or
`reason=workers-late`.

When the next blk failure appears:
- `reason=workers-late` → reads were slow, no data defect; track pacing
  separately, do NOT write a driver fix.
- `reason=checksum-mismatch` → genuine DDR-775/776 data defect; write a DDR for
  the driver-side root cause.

Also fix these two proven S2 defects **after** item 47 is resolved (they are
independent of the intermittent's root cause; draft now, push after three greens):
- Bound the virtio-blk completion wait (currently unbounded → hangs forever on
  a missed completion).
- Convert `slot_waiter` to a FIFO wait-list for >VBLK_NREQ concurrent submitters.

### 6.2 Storage / FS

Work through in this order. Each item requires CI-green before the next.

1. **Provisioned SFS as default boot root** — make `build/sfsroot.img` / the
   `QEMU_SFSROOT` path the normal boot; the SFS destructive self-tests must move
   to a scratch disk or be gated behind `QEMU_SFS_SELFTEST=1`. DDR-770/771
   provide the foundation.

2. **FAT32 multi-cluster read fix** — `execve` of a large musl-C ELF from FAT32
   corrupts (likely the `read_cluster_chain` path for >1-cluster ELFs). Diagnose,
   root-cause, and fix. This unblocks PRISM `run`, init `fork`+`execve` respawn,
   and agent `execve`-on-respawn.

3. **SFS on-disk free-tree persistence** — DDR-762-v2 implemented in-memory
   reclaim. The on-disk free-extent B+tree (allocated at `sfs_format` time) is
   required for reclaim to survive reboots.

4. **SFS B+tree CoW GC** — the B+tree is append/tombstone only; a compaction pass
   (rewrite leaf, update parent pointers) prevents unbounded dead-slot growth.

5. **SFS extent overflow / large files** — extend beyond 4 inline extents. Add
   indirect extent blocks or enlarge the inline array.

6. **`mkfs.sfs` >512 slots / deeper trees** — remove the flat slot array ceiling
   with a proper recursive B+tree builder.

7. **SFS free-space quotas / per-mount limits** — per ADR-032 deferred. Implement
   per-process-root-mount token-bucket quota.

### 6.3 Networking (NET-C and beyond)

1. **`epoll` / `select` integration for proxy sockets** — `SYS_SOCK_READ` parks
   on `sti;hlt;cli`. Integrate proxy-socket readiness into the `epoll` interest
   table; trigger from the recv callback.

2. **UDP send / raw socket API** — the ring-3 socket NSI covers TCP only.

3. **`SYS_NET_REVOKE` / CAP_NET policy reload** — DDR-734 is install-only.
   Add a `SYS_NET_REVOKE` (sovereign-only) for runtime allowlist changes without
   reboot.

4. **True peer loopback / TAP netdev** — NET-A noted real loopback beyond QEMU
   SLIRP requires a tap or socket netdev (NET-C).

5. **IPv6** — tracked in the masterplan as a future layer. Build after NET-C is
   stable.

6. **TLS shim** — the Ollama agent path uses plaintext HTTP/1.1. Add minimal TLS
   (mbedTLS or equivalent minimal lib, subject to the no-out-of-tree-libs wall for
   the OS image).

### 6.4 Userspace / shell

1. **`argv`/`envp` marshalling in `sys_execve`** — ADR-021/ADR-022 deferred this.
   Implement full NULL-terminated `char**` copying via `copyin`, placed on the
   initial user stack below `auxv`.

2. **PRISM RX line discipline / echo / readline** — add echo, backspace, Ctrl-C
   → SIGINT, and line-buffer discipline to the console input path.

3. **PRISM `run` re-enable** — after FAT32 multi-cluster fix (§6.2-2), re-enable
   init `fork`+`execve` respawn of PRISM.

4. **PRISM pipes / redirection / quoting / job control / scripting** — shell
   grammar parser, pipe plumbing (`fork`+`dup2`+`execve`), `&` background jobs,
   `wait` builtin.

5. **`SYS_MPROTECT`** — required for JIT, dynamic linker segment permission
   changes, and POSIX compliance.

6. **`SYS_POLL`** — required for many POSIX programs; complement to `epoll`.

7. **`SYS_FUTEX`** — required for musl's `pthread_mutex` / `pthread_cond`.

8. **`pthread` / ring-3 threading** — `clone(CLONE_VM|CLONE_FILES|CLONE_THREAD)`,
   per-thread stack, `SYS_FUTEX`, TLS-per-thread setup.

9. **6-arg `sys_mmap` ABI widening** — ADR-022 deferred (every 5b call fits ≤4
   args). Required for `mmap` with fd/offset for file-backed mappings.

10. **`mmap` file-backed mappings** — MAP_ANON only today. Add: page-fault handler
    calling into VFS, dirty tracking, `msync`. Required for dynamic linking.

11. **Dynamic linking** — `ld.so`/musl dynamic linker, `.so` support in the ELF
    loader, `SYS_MMAP` file-backed.

12. **`io_uring` completions** — add `OP_FSYNC`, `OP_OPENAT`, completion-event
    notifications (eventfd), SQE chaining, fixed buffers.

13. **`epoll` blocking wait** — PROC-B is non-blocking. Add blocking `epoll_wait`
    (park caller, wake on readiness from pipe/socket recv path).

14. **`SYS_SIGACTION` full POSIX** — `SA_RESTART`, `SA_SIGINFO`, `sigprocmask`,
    `sigaltstack`, `SIGCHLD` generation on child exit.

15. **PRISM `ls -R` / `ps` full** — per-process open-fd listing, recursive `ls`,
    signal-mask display in `ps`.

### 6.5 Compositor / UI (Layer 7 remaining)

1. **PS/2 scancode modifier keys** — multi-byte scancodes (F-keys, arrow keys,
   Insert/Delete/Home/End, numpad) and modifier tracking (Alt, Ctrl, Meta/Super).
   Super+M (brief §3 sovereign/manual toggle) is the priority modifier.

2. **Super+M physical binding** — once modifier plumbing is done, bind Super+M to
   `SYS_SET_MODE` in the compositor. This is the brief's §3 specification.

3. **Alt-Tab with modifier plumbing** — DDR-720 used plain Tab. Upgrade to Alt+Tab
   once modifier tracking is in place.

4. **Compositor hotkey Ctrl+Alt+T** — launch a PRISM terminal window.

5. **Per-window restore from dock / taskbar** — DDR-717's `r` key restores ALL
   minimized windows. Add a taskbar with per-window restore tiles.

6. **Window maximize — full-screen at real display size** — DDR-719 caps at
   512×512. Full-screen requires a larger surface budget or a scanout-override
   path.

7. **Pointer resize handles — all edges** — DDR-718 covers bottom-right corner
   only. Add left/top/other-edge handles plus minimum window size enforcement.

8. **`SURF_EV_CLOSE` notification** — send a close event from the compositor to
   the owner before forcible `SYS_SURFACE_CLOSE`, so the owner can save state
   (the Wayland `xdg_toplevel.close` shape).

9. **Compositor double-map `PTE_SW_SHARED` audit** — verify the compositor's
   `SYS_SURFACE_CMAP` double-map is correctly marked `PTE_SW_SHARED` so address-
   space teardown (DDR-729) never double-frees surface frames.

10. **OKLab horizon bands / animated mesh** — DDR-716 deferred mesh animation and
    horizon bands. Implement per-ambiance animated horizon + the DAY-ambiance mesh.

11. **vDSO callable reader (`vdso_entry.asm`)** — IMP-C deferred the ring-3-
    callable seqlock reader. Implement it so multi-word consistent wall-clock reads
    don't require a syscall.

### 6.6 Kernel / SMP / hardening

1. **Scheduler timed-block primitive** — `sched_block_on_timeout(&lk, deadline)`
   — the correct fix for virtio-blk's unbounded wait and any other bounded-wait
   need. Implement after item 47 is resolved (timer must be proven reliable first).

2. **I/O APIC migration (DDR-714 stage D)** — all virtio devices and NVMe use
   MSI-X; the 8259 PIC is still live for ISA lines (keyboard IRQ1, COM1 IRQ4).
   Migrate to the I/O APIC: disable 8259 in virtual-wire mode, route ISA IRQs
   through I/O APIC GSIs.

3. **SMEP / SMAP enablement** — IMP-A covered Spectre/Meltdown MSRs. Enable SMEP
   (Supervisor Mode Execution Prevention) and SMAP (Supervisor Mode Access
   Prevention). The `copyin`/`copyout` discipline already provides the structural
   requirement; add `CLAC`/`STAC` around every copyin/copyout call site.

4. **Kernel-self W^X — identity alias removal** — the kernel text is writable
   through the 2 MiB identity alias. Call `vmm_protect_kernel()` on the identity
   alias pages as R+NX (or remove the alias once relocation is stable).

5. **`#MC` machine-check handler** — add a minimal `#MC` handler that panics with
   register state; a bare-metal crash would otherwise triple-fault.

6. **KASLR** — kernel loads at fixed physical address 0x400000. Add KASLR for
   hardening. (Track as a future ADR; do not start before §6.6-1..5 are done.)

7. **Per-CPU `sched_exit` / zombie reap under full SMP** — the reaper thread runs
   BSP-only. Add cross-CPU zombie queue or per-CPU reap sweep.

8. **Spinlock contention instrumentation** — add `lock_stat` (hold-time + contention
   counters) to identify hot spinlocks under pathological workloads.

9. **`smoke-rqstress` determinism** — per-CPU runqueue migration stress testing
   under real user workloads beyond the 24-thread storm. Ensure the gate is
   deterministic across 20× runs before moving on.

### 6.7 AETHER / agent layer

1. **AETHER audit ring persistence** — the 4096-entry audit ring is in-memory
   only. Persist it to SFS (`/etc/aether/audit.log`). Requires §6.2-1 (SFS as
   default root) first.

2. **Agent `execve`-on-respawn from SFS** — the AETHER daemon spawns agents via
   embedded ELF bytes. Change to `execve` from the SFS agent image store
   (`/agents/kryos.elf` etc.). Blocked on §6.2-2 (FAT32 multi-cluster fix) or
   unblocked once SFS is the agent root.

3. **Multi-agent concurrency arbitration** — the AETHER queue is one global
   256-entry ring. Add per-agent quota + priority scheduler for the action queue.

4. **Per-agent live-metrics panel richer display** — DDR-737 closed the hardening
   campaign. Add CPU% sparkline, memory-used graph, and action-rate histogram to
   the agent panel in the compositor.

5. **`SYS_AGENT_ROSTER` / `SYS_AGENT_METRICS` — per-slot liveness continuity** —
   ensure metrics are retained across agent restarts (current: slot retains last
   dead agent's counts, which is correct; verify no regression on respawn).

6. **`/etc/aether/config` FAT fallback sentinel forbidden** — DDR-732/761 migrated
   to SFS. Ensure `smoke-aethercfg` explicitly FORBIDs the `PRADYOS_AETHER_CFG_DEFAULT`
   sentinel so a FAT fallback is always a gate failure.

### 6.8 Platform / ISA extensions

1. **ARM64 (AArch64) port** — AArch64 boot protocol, EL1/EL0, GIC instead of
   LAPIC/8259, MMU with 4-level translation. Track in `docs/platform_profiles.md`.
   Begin after all x86_64 items in §6.1–6.7 are CI-green.

2. **RISC-V 64 port** — similarly, after ARM64 work is underway.

3. **3-lane NAS storage** — deferred since phase 3. Implement after SFS is stable
   as the default root (§6.2-1).

4. **`clone(CLONE_VM)` for POSIX threading** — IMP-D shipped COW fork. Full
   `clone(CLONE_VM|CLONE_FILES|CLONE_THREAD)` is the prerequisite for pthreads
   (§6.4-8). Implement together with `SYS_FUTEX`.

### 6.9 Release (v1.0.0)

Work through these only after §6.1–6.8 are CI-green.

1. **ISO images × 4** — x86_64 (primary), ARM64, RISC-V64, and a virtualisation
   bundle. Per `docs/BUILD_TRACKER.md` Section 9. The two pre-approved exceptions:
   - **Item 26 — Intel HDA audio** — OPTIONAL; skip and log as "deferred, optional".
   - **Item 41 — Wayland/wlroots compositor** — PRE-APPROVED SUPERSEDED by the
     in-house C framebuffer compositor. Log as "superseded, not required".

2. **`prad` package manager** — NSI 88–90 per `docs/BUILD_TRACKER.md` TASK 18.

3. **Full invariant gate suite** — 20× each intermittent-class gate, all green, no
   excluded gates except the two pre-approved exceptions above.

4. **Tag `v1.0.0`** — only after every CI gate (minus the two exceptions) is
   green, `main` = `dev/phase1`, and the ISO images are validated.

---

## 7. Hygiene gates — must pass before EVERY commit

1. `build/kernel.bin` warning-clean (`-Werror` for clang + nasm).
2. No forbidden sentinels in the build output (run `make ci-probe-rodata-check`).
3. No `[BUG]` lines in the serial log.
4. `make smoke-blkmq` exits rc=0.
5. `make ci-shard-check` passes (every `smoke-*` target assigned to exactly one
   shard, no target named that the Makefile does not define).

---

## 8. Standing rules (do not re-derive these)

- **A gate's timeout is a claim about how long the system takes.** When a gate
  fails on "pattern not found", check elapsed against the window before reading
  code. Use `make TIMEOUT_S=<n> smoke-<gate>` to override at the make level
  (recipe-level `TIMEOUT_S=` beats the environment).
- **An address does not identify a binary when every binary loads at the same
  base.** Confirm which ELF is running before disassembling anything.
- **A revert is not verified until the gate is re-run** after the revert.
- **Gate logs go under `build/gatelogs/`** (WSL wipes `/tmp` mid-run; CI default
  unchanged).
- **`make ci-probe-rodata-check` catches writable-global faults** before they
  become QEMU silent failures. Run it before registering any new probe ELF.
- **Never run two QEMU instances concurrently on this host** — they race the
  image write-lock and produce false results.
- **`pgrep qemu-system-x86_64` must return empty** before a local gate run.
- **`IRQF_PERCPU` has no analogue in this kernel.** Do not attempt to apply Linux
  driver patterns that have no mapping here.
- **`kmalloc` does not zero.** Every new `struct tcb` field needs an explicit
  initialiser in `sched_create`. See memory `tcb-fields-not-zeroed`.
- **Explicitly deferred — do not pull forward under any circumstance:**
  - `arch/aarch64` and `arch/riscv64` ports before §6.1–6.7 are complete.
  - Phase 10 quantum layer (QAL, virtual QPU, QAOA scheduler, hybrid API).
  - Rust rewrite of any component.
  - Cloud bridge activation (DDR-793) — flag and ask if a queue item has a hard
    dependency on it.
  - Apple Silicon / m1n1 path (until ARM64 port is scoped as its own ADR).
