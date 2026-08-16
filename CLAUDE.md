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

### 5c. Context-limit protocol — CHECKPOINT AND CONTINUE, NEVER STOP

**There is no context level at which you stop working and surface a response.
The OS is not built yet. You do not stop until it is.**

When you notice context is high (use your own judgment on when this is):

1. Finish the current atomic operation (one function, one file — not a whole
   task).
2. Build and run the gate for what was just written.
3. Commit with an honest message (pass or fail, state which).
4. Append a checkpoint block to `SESSION_HANDOFF.md`.
5. Push both the work commit and the `SESSION_HANDOFF.md` commit.
6. **Immediately start the next task. Do not stop. Do not surface a response
   to the user. Continue working.**

The checkpoint exists so work is not lost — not as permission to stop.
**Stopping and reporting to the user is NEVER the correct action mid-build.**
The only time you surface a response is when one of the §5d stop conditions
is genuinely met.

### 5d. Autonomous task loop — KEEP GOING UNTIL EVERYTHING IS BUILT

Work through the backlog in §6 below **without waiting for human confirmation
between tasks.** The only valid stop conditions are:

1. A gate fails that is NOT the active repair target AND no fix is obvious
   AND you have already tried two independent approaches.
2. A build error that requires an architectural decision not covered by existing
   ADRs, AND you have already read all related ADRs and cannot resolve it.
3. A CI run is in flight and local QEMU would contend with it — in this case
   work on code-reading and DDR-writing tasks only, then resume QEMU work when
   CI clears. **This is not a stop — it is a task switch.**

**For everything else: keep going.** Do not stop at the end of a task and ask
what to do next. Read §6, pick the next unbuilt item, and start it immediately.
Do not surface a response to the user between tasks. Do not ask for confirmation.
Do not announce task completions. Just keep building.

**If you think you have hit a stop condition, check this list:**
- "I finished a task" — NOT a stop condition. Start the next task.
- "I am waiting for CI" — NOT a stop condition. Work on code reading / DDRs.
- "I am not sure what to do next" — NOT a stop condition. Read §6 and start
  the next unbuilt item.
- "I should report my progress" — NOT a stop condition. Keep working.
- "Context is high" — NOT a stop condition. Checkpoint per §5c and continue.
- "The user hasn't confirmed" — NOT a stop condition. §5d forbids waiting.

---

## 6. The complete build backlog — work through this in order

The goal is to build **everything** in this list. Nothing here is optional except
the two pre-approved exceptions noted. Work items are ordered by dependency;
do not start an item whose prerequisites are not yet CI-green.

### 6.0 Mandatory pre-conditions (enforce every session)

- **§6.0-A — CI-in-flight rule:** before running QEMU locally, confirm no CI run
  is in flight on `dev/phase1`. Check with `gh run list --branch dev/phase1
  --limit 5`. If a run is in flight, do NOT stop — work on code-reading and
  DDR-writing tasks until CI clears, then resume QEMU work immediately.
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

1. **`epoll` / `select` integration for proxy sockets**
2. **UDP send / raw socket API**
3. **`SYS_NET_REVOKE` / CAP_NET policy reload**
4. **True peer loopback / TAP netdev**
5. **IPv6**
6. **TLS shim**

### 6.4 Userspace / shell

1. **`argv`/`envp` marshalling in `sys_execve`**
2. **PRISM RX line discipline / echo / readline**
3. **PRISM `run` re-enable** (after §6.2-2)
4. **PRISM pipes / redirection / quoting / job control / scripting**
5. **`SYS_MPROTECT`**
6. **`SYS_POLL`**
7. **`SYS_FUTEX`**
8. **`pthread` / ring-3 threading**
9. **6-arg `sys_mmap` ABI widening**
10. **`mmap` file-backed mappings**
11. **Dynamic linking**
12. **`io_uring` completions**
13. **`epoll` blocking wait**
14. **`SYS_SIGACTION` full POSIX**
15. **PRISM `ls -R` / `ps` full**

### 6.5 Compositor / UI (Layer 7 remaining)

1. **PS/2 scancode modifier keys**
2. **Super+M physical binding**
3. **Alt-Tab with modifier plumbing**
4. **Compositor hotkey Ctrl+Alt+T**
5. **Per-window restore from dock / taskbar**
6. **Window maximize — full-screen at real display size**
7. **Pointer resize handles — all edges**
8. **`SURF_EV_CLOSE` notification**
9. **Compositor double-map `PTE_SW_SHARED` audit**
10. **OKLab horizon bands / animated mesh**
11. **vDSO callable reader (`vdso_entry.asm`)**

### 6.6 Kernel / SMP / hardening

1. **Scheduler timed-block primitive** (after item 47 resolved)
2. **I/O APIC migration (DDR-714 stage D)**
3. **SMEP / SMAP enablement**
4. **Kernel-self W^X — identity alias removal**
5. **`#MC` machine-check handler**
6. **KASLR** (after §6.6-1..5)
7. **Per-CPU `sched_exit` / zombie reap under full SMP**
8. **Spinlock contention instrumentation**
9. **`smoke-rqstress` determinism** (20× green)

### 6.7 AETHER / agent layer

1. **AETHER audit ring persistence** (needs §6.2-1)
2. **Agent `execve`-on-respawn from SFS** (needs §6.2-2)
3. **Multi-agent concurrency arbitration**
4. **Per-agent live-metrics panel richer display**
5. **`SYS_AGENT_ROSTER` / `SYS_AGENT_METRICS` liveness continuity**
6. **`/etc/aether/config` FAT fallback sentinel forbidden**

### 6.8 Platform / ISA extensions

1. **ARM64 port** (after §6.1–6.7 CI-green)
2. **RISC-V 64 port** (after ARM64 underway)
3. **3-lane NAS storage** (after §6.2-1)
4. **`clone(CLONE_VM)` for POSIX threading** (with §6.4-7+8)

### 6.9 Release (v1.0.0)

1. **ISO images × 4** (pre-approved exceptions: Intel HDA audio → "deferred,
   optional"; Wayland/wlroots → "superseded, not required")
2. **`prad` package manager**
3. **Full invariant gate suite — 20× each intermittent-class gate**
4. **Tag `v1.0.0`** — only after every CI gate (minus two exceptions) is green,
   `main` = `dev/phase1`, ISO images validated.

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
- **Stray-QEMU check: `pgrep -f "[q]emu-system-x86_64"`** (bracket form ONLY —
  the name form `pgrep qemu-system-x86_64` is permanently broken due to Linux's
  15-character comm truncation).
- **`IRQF_PERCPU` has no analogue in this kernel.** Do not attempt to apply Linux
  driver patterns that have no mapping here.
- **`kmalloc` does not zero.** Every new `struct tcb` field needs an explicit
  initialiser in `sched_create`. See memory `tcb-fields-not-zeroed`.
- **Auth:** if `git push` fails 403 as the wrong user, run
  `gh auth switch --user prady4the4bady` before retrying. Do this at the start
  of every session as a precaution.
- **DDR number allocation:** free range starts at DDR-936. Before allocating a
  new DDR number, run `ls docs/ddr/ docs/decisions/ | grep DDR-<N>` to confirm
  it is unoccupied in both directories. Occupied: DDR-885..935 (see
  `docs/ddr/DDR-NUMBERING-MAP.md` for the collision-resolution record).
- **Explicitly deferred — do not pull forward under any circumstance:**
  - `arch/aarch64` and `arch/riscv64` ports before §6.1–6.7 are complete.
  - Phase 10 quantum layer.
  - Rust rewrite of any component.
  - Cloud bridge activation (DDR-793).
  - Apple Silicon / m1n1 path.
