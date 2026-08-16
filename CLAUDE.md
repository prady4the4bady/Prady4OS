# PRADYOS — Claude Code session rules (CLAUDE.md)

PRADYOS Sovereign Edition: a from-scratch, bare-metal, AI-native OS for x86_64
(reference ISA). Work proceeds in strict layer/slice order; a slice ships when it
is correct, not when it is fast. **This file governs every session. Read it in
full before touching anything.**

---

## 0. Hard-won lessons — READ BEFORE TOUCHING ANYTHING

These are facts established by measurement and confirmed by CI. Re-deriving them
wastes context. Accept them as invariants.

### 0.1 g_ticks freeze — DDR-887 (commit d72bd93) — DO NOT REVERT

`g_ticks` froze under `-smp 4` because `sched_tick` called `schedule()` with
interrupts disabled, which is non-reentrant against the LAPIC timer. The fix
introduces a brief `sti; pause; cli` window so the tick can advance while
holding the scheduler lock.

**Pattern (binding — do not change without a new ADR):**
```c
/* sched_tick: allow one tick to advance before scheduling */
__asm__ volatile("sti; pause; cli" ::: "memory");
```

The flag `g_in_switch` suppresses reentrant `schedule()` calls for the duration
of the `sti;pause;cli` window. This is the correct mechanism. The alternative
(clearing `g_in_switch` mid-spin) re-opens the reentrancy hazard that
`sched.c:983-989` warns about. Never do that.

**Measurement (DDR-893, commit 0c847bc) confirmed:**
- Mean spin/call ≈19 iterations — the wait IS short, as the comment claims.
- Contention is boot-phase only — exactly zero from t=2000 onward.
- Total suppression ≈0.4% of a window — cannot cause seconds-long gate overruns.
- Reading (B) ("preemption suppression is the CI blocker") is REFUTED.
- Reading (A) ("pre-existing flakes the freeze was hiding") is the surviving
  hypothesis for post-DDR-887 CI failures.

### 0.2 Items 47 and 48 closure procedure (binding)

**Item 47 — g_ticks stall:** NEVER guess a fix. The `[boot-load]`/`[boot-stamp]`
instrument (commit `e49a23f`, ADR-037) must produce a failing-run capture first.
The fix follows from the tick-gap measurement:
- Gap ≈0 → LAPIC not firing on APs → investigate AP LAPIC init.
- Gap large, ticks advancing → scheduler starvation → investigate percpu rq.
Write the DDR before any fix code.

**Item 48 — virtio-blk workers-late:** per DDR-886 (commit `2dc1bad`), the
probes now print `reason=` on failure.
- `reason=workers-late` → scheduling issue, NOT a driver bug. Do NOT write a
  virtio-blk driver change. The confirmed root cause (DDR-934) is
  `sched_create` NULL return under heap pressure — add NULL-check + KASSERT.
- `reason=checksum-mismatch` → genuine data defect → write a driver DDR.

Items 47 and 48 are INDEPENDENT. One DDR per root cause (§6.0-C).
Both must be CI-green before `main` can be promoted.

### 0.3 Stray-QEMU guard — the name form is permanently broken

`pgrep qemu-system-x86_64` returns ZERO MATCHES whether or not QEMU is running.
Linux truncates `comm` to 15 characters; `qemu-system-x86_64` is 18 characters.
Every "no stray QEMU" observation recorded against the name form was vacuous.
Two retracted root causes in this project's history trace to concurrent-QEMU
contamination that the broken guard failed to catch.

**Correct form — always and only:**
```
pgrep -f "[q]emu-system-x86_64"
```
The bracket avoids self-matching the invoking shell.

### 0.4 DDR number collision — always verify before allocating

In August 2026 a session allocated DDR-885..916 without checking, colliding with
pre-existing DDRs in `docs/decisions/`. The collision-resolution map is at
`docs/ddr/DDR-NUMBERING-MAP.md`. The free range is **DDR-936+**.

Before allocating ANY DDR number:
```
ls docs/ddr/ docs/decisions/ | grep DDR-<N>
```
Must return empty in BOTH directories.

### 0.5 Geometry-publishing pattern — do not hardcode pixel coordinates in gates

DDR-910, DDR-926, DDR-929, DDR-935 all trace to the same root: gate scripts
hardcoded absolute tablet coordinates that drifted with window position.

**Rule:** the compositor MUST publish geometry (`PRADYOS_WM_GEOM`) and gate
injectors MUST read it. Adding a new interactive gate? Add the corresponding
`PRADYOS_WM_GEOM` field first. The field format is:
```
PRADYOS_WM_GEOM id=<N> title=<T> close=X,Y min=X,Y rz=X,Y dg=X,Y
```
Parsers must isolate each field before splitting on `,` — appending a new
field to the end of the line corrupts earlier parsers that use `##*,`
(takes everything after the LAST comma in the entire line).

### 0.6 `kmalloc` does not zero — every new TCB field needs an initialiser

`kmalloc` returns uninitialised memory. Every new field added to `struct tcb`
must be explicitly initialised in `sched_create`. Failure mode: intermittent
failures under SMP where a new field contains stack garbage from a prior
allocation. See memory node `tcb-fields-not-zeroed`.

### 0.7 Aggregate vs per-call metrics — always measure both

DDR-890 claimed reading (B) confirmed from spin totals alone (2–3 million/window).
DDR-893 added call counts and refuted it: mean spin/call was ~19, not ~1 million.
A cumulative counter cannot distinguish one long event from many short ones.

**Rule:** any performance claim must include BOTH a total AND a per-event metric
(call count, event count, or similar denominator). A total without a denominator
is not evidence.

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

1. Run `gh auth switch --user prady4the4bady` (prevents 403 push failures).
2. Read `SESSION_HANDOFF.md` in full.
3. Read `docs/build_status.md` to confirm the current tip SHA and gate count.
4. Run `graph_session_primer()`.
5. **Do NOT run the full gate suite before starting work.** Gates are run after
   the fix, not before.
6. Identify `CURRENT_ACTIVE_TASK` from `SESSION_HANDOFF.md` and start it
   immediately.

### 5b. The active-task gate exception

If the current task is fixing a specific failing gate, that gate being red does
NOT block starting the task. Do not waste context verifying a known-broken gate
is still broken. Run the gate after implementing the fix.

### 5c. Context-limit protocol — CHECKPOINT AND CONTINUE, NEVER STOP

**There is no context level at which you stop working and surface a response.
The OS is not built yet. You do not stop until it is.**

When you notice context is high:

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
- "I finished a task" — NOT a stop. Start the next task.
- "I am waiting for CI" — NOT a stop. Work on code reading / DDRs.
- "I am not sure what to do next" — NOT a stop. Read §6 and start next item.
- "I should report my progress" — NOT a stop. Keep working.
- "Context is high" — NOT a stop. Checkpoint per §5c and continue.
- "The user hasn't confirmed" — NOT a stop. §5d forbids waiting.

---

## 6. The complete build backlog — work through this in order

The goal is to build **everything** in this list. Nothing here is optional except
the two pre-approved exceptions noted. Work items are ordered by dependency;
do not start an item whose prerequisites are not yet CI-green.

### 6.0 Mandatory pre-conditions (enforce every session)

- **§6.0-A — CI-in-flight rule:** before running QEMU locally, run
  `gh run list --branch dev/phase1 --limit 5`. If a run is in flight: do NOT
  stop — switch to code-reading / DDR-writing tasks and resume QEMU when clear.
- **§6.0-B — Instrument-first rule:** items 47 and 48 must not be fixed on
  hypothesis. Capture the failing run's instrument output first.
- **§6.0-C — One DDR per root cause:** items 47 and 48 are confirmed independent.
  Do not conflate them.
- **§6.0-D — Fix on confirmed data only:** no virtio-blk driver change until
  `reason=checksum-mismatch` appears with DDR-886 probes active. Until then,
  only diagnosability improvements are permitted.

### 6.1 Active intermittents — fix before promotion, 3 consecutive CI greens required

**Item 47 — g_ticks stall** — see §0.2 for the exact closure procedure.
Do not attempt a fix without a failing-run capture from the
`[boot-load]`/`[boot-stamp]` instrument.

**Item 48 — virtio-blk workers-late** — see §0.2 for the exact closure procedure.
Confirmed root cause: `sched_create` NULL return (DDR-934). Fix: NULL-check +
KASSERT in `blkmq_proof` and `smp_blk_integrity`, plus `smp_resched_all()` after
worker spawn (matching `rqstress_proof` pattern at main.c:584-585).

Also fix after item 47 (S2 defects, independent of intermittent root cause):
- Bound the virtio-blk completion wait (currently unbounded).
- Convert `slot_waiter` to FIFO wait-list for >VBLK_NREQ concurrent submitters.

### 6.2 Storage / FS

Each item requires CI-green before the next starts.

1. **Provisioned SFS as default boot root** — gate `sfs_format` at main.c:1128
   behind `probe_enabled()` (fw_cfg / `QEMU_PROBES` path). Update the 12 gates
   that assert on `[sfs]` self-test sentinels. New gate: `smoke-sfs-boot-root`
   prints `PRADYOS_SFS_ROOT_OK`.

2. **FAT32 multi-cluster read fix** — `execve` of a large musl-C ELF from FAT32
   corrupts. Root-cause via `read_cluster_chain` path for >1-cluster ELFs.
   New probe `fat32_multicluster.c`, pattern 7n+3 across cluster boundary.
   Gate: `smoke-fat32-multicluster`.

3. **SFS on-disk free-tree persistence** — gate: `smoke-sfs-persist`.

4. **SFS B+tree CoW GC** — gate: `smoke-sfs-gc`.

5. **SFS extent overflow / large files** — gate: `smoke-sfs-largefile`.

6. **`mkfs.sfs` >512 slots / deeper trees** — gate: `smoke-sfs-deepslot`.

7. **SFS free-space quotas / per-mount limits** — gate: `smoke-sfs-quota`.

### 6.3 Networking (NET-C and beyond)

1. **`epoll` / `select` integration for proxy sockets**
2. **UDP send / raw socket API**
3. **`SYS_NET_REVOKE` / CAP_NET policy reload**
4. **True peer loopback / TAP netdev**
5. **IPv6** (after NET-C stable)
6. **TLS shim** (mbedTLS or equivalent, no out-of-tree libs in OS image)

### 6.4 Userspace / shell

1. **`argv`/`envp` marshalling in `sys_execve`**
2. **PRISM RX line discipline / echo / readline**
3. **PRISM `run` re-enable** (after §6.2-2)
4. **PRISM pipes / redirection / quoting / job control / scripting**
5. **`SYS_MPROTECT`**
6. **`SYS_POLL`**
7. **`SYS_FUTEX`**
8. **`pthread` / ring-3 threading** (`clone(CLONE_VM|CLONE_FILES|CLONE_THREAD)`)
9. **6-arg `sys_mmap` ABI widening**
10. **`mmap` file-backed mappings** (page-fault handler, dirty tracking, `msync`)
11. **Dynamic linking** (`ld.so` / musl dynamic linker, `.so` in ELF loader)
12. **`io_uring` completions** (`OP_FSYNC`, `OP_OPENAT`, eventfd, SQE chaining)
13. **`epoll` blocking wait** (park caller, wake on readiness)
14. **`SYS_SIGACTION` full POSIX** (`SA_RESTART`, `SA_SIGINFO`, `sigprocmask`,
    `sigaltstack`, `SIGCHLD` on child exit)
15. **PRISM `ls -R` / `ps` full** (open-fd listing, recursive ls, signal-mask)

### 6.5 Compositor / UI (Layer 7 remaining)

1. **PS/2 scancode modifier keys** (F-keys, arrows, Alt, Ctrl, Meta/Super)
2. **Super+M physical binding** → `SYS_SET_MODE` (brief §3 sovereign toggle)
3. **Alt-Tab with modifier plumbing** (upgrade from plain Tab, DDR-720)
4. **Compositor hotkey Ctrl+Alt+T** → launch PRISM terminal window
5. **Per-window restore from dock / taskbar** (DDR-717 restores all; add per-tile)
6. **Window maximize — full-screen at real display size** (DDR-719 caps at 512×512)
7. **Pointer resize handles — all edges** (DDR-718 covers bottom-right only)
8. **`SURF_EV_CLOSE` notification** (owner saves state before forced close)
9. **Compositor double-map `PTE_SW_SHARED` audit**
10. **OKLab horizon bands / animated mesh** (DDR-716 deferred mesh + horizon)
11. **vDSO callable reader (`vdso_entry.asm`)** (ring-3 seqlock reader, IMP-C)

### 6.6 Kernel / SMP / hardening

1. **Scheduler timed-block primitive** `sched_block_on_timeout(&lk, deadline)`
   — implement AFTER item 47 is CI-green (timer must be proven reliable first).
2. **I/O APIC migration** (DDR-714 stage D — disable 8259, route ISA IRQs)
3. **SMEP / SMAP enablement** (`CLAC`/`STAC` around every copyin/copyout)
4. **Kernel-self W^X — identity alias removal** (`vmm_protect_kernel()`)
5. **`#MC` machine-check handler** (panic with register state)
6. **KASLR** (after §6.6-1..5 done)
7. **Per-CPU `sched_exit` / zombie reap under full SMP**
8. **Spinlock contention instrumentation** (`lock_stat` hold-time + contention)
9. **`smoke-rqstress` determinism** (20× green before moving on)

### 6.7 AETHER / agent layer

1. **AETHER audit ring persistence** — persist to SFS (`/etc/aether/audit.log`).
   Requires §6.2-1 first.
2. **Agent `execve`-on-respawn from SFS** (`/agents/kryos.elf` etc.).
   Blocked on §6.2-2 or SFS as agent root.
3. **Multi-agent concurrency arbitration** (per-agent quota + priority queue)
4. **Per-agent live-metrics panel** (CPU% sparkline, memory graph, action-rate
   histogram)
5. **`SYS_AGENT_ROSTER` / `SYS_AGENT_METRICS` liveness continuity**
6. **`/etc/aether/config` FAT fallback sentinel FORBIDDEN** in `smoke-aethercfg`

### 6.8 Platform / ISA extensions

1. **ARM64 (AArch64) port** — begin ONLY after all §6.1–6.7 items are CI-green.
2. **RISC-V 64 port** — after ARM64 underway.
3. **3-lane NAS storage** — after §6.2-1 stable.
4. **`clone(CLONE_VM)` for POSIX threading** — with §6.4-7+8.

### 6.9 Release (v1.0.0)

1. **ISO images × 4** — x86_64, ARM64, RISC-V64, virtualisation bundle.
   Pre-approved exceptions:
   - Intel HDA audio → "deferred, optional"
   - Wayland/wlroots → "superseded, not required"
2. **`prad` package manager** (NSI 88–90, docs/BUILD_TRACKER.md TASK 18)
3. **Full invariant gate suite** — 20× each intermittent-class gate, all green.
4. **Tag `v1.0.0`** — ONLY after every CI gate (minus two exceptions) is green,
   `main` = `dev/phase1`, ISO images validated.

---

## 7. Hygiene gates — must pass before EVERY commit

1. `build/kernel.bin` warning-clean (`-Werror` for clang + nasm).
2. No forbidden sentinels in build output (`make ci-probe-rodata-check`).
3. No `[BUG]` lines in the serial log.
4. `make smoke-blkmq` exits rc=0.
5. `make ci-shard-check` passes.
6. `make smoke-rqstress-liveness` exits rc=0.
7. `make smoke-blk-integrity` exits rc=0.

---

## 8. Standing rules (do not re-derive these)

- **A gate's timeout is a claim about how long the system takes.** Check elapsed
  vs window before reading code. `make TIMEOUT_S=<n> smoke-<gate>` to override.
- **An address does not identify a binary** when every binary loads at the same
  base. Confirm which ELF is running before disassembling.
- **A revert is not verified until the gate is re-run** after the revert.
- **Gate logs go under `build/gatelogs/`** — WSL wipes `/tmp`.
- **`make ci-probe-rodata-check`** before registering any new probe ELF.
- **Never run two QEMU instances concurrently** — they race the image write-lock.
- **Stray-QEMU: `pgrep -f "[q]emu-system-x86_64"`** (bracket form only — see §0.3).
- **`IRQF_PERCPU` has no analogue in this kernel.**
- **`kmalloc` does not zero** — see §0.6.
- **Auth:** `gh auth switch --user prady4the4bady` at session start and on any
  403 push failure.
- **DDR allocation:** DDR-936+ only. Verify unoccupied in both `docs/ddr/` AND
  `docs/decisions/` before allocating — see §0.4.
- **Geometry in gates:** use `PRADYOS_WM_GEOM` fields, never hardcoded pixels —
  see §0.5.
- **Performance claims need a denominator** — see §0.7.
- **Explicitly deferred — do not pull forward:**
  - `arch/aarch64` / `arch/riscv64` before §6.1–6.7 complete.
  - Phase 10 quantum layer.
  - Rust rewrite of any component.
  - Cloud bridge activation (DDR-793).
  - Apple Silicon / m1n1 path.
  - CMake/Makefile hybrid (awaiting operator sign-off).
