# PRADYOS — OPERATOR DIRECTIVE — 2026-08-23 (SUPERSEDES ALL PRIOR SCOPE LIMITS)

**Read this file in full before starting any session. This is a binding scope
and deadline change, not a suggestion.**

---

## 1. THE DEADLINE

**New hard deadline: 2026-08-28 23:59 UTC.**

This replaces the 2026-08-24 deadline. It is a real extension, not a soft
target, and it will not move again. `v1.0.0` must be tagged on `main` with the
FULL scope below complete — not a minimal/demo release — by this deadline.

## 2. THE SCOPE CHANGE — NO MORE SILENT DEFERRALS

Prior sessions correctly used the "PRE-APPROVED EXCEPTIONS" table in
CLAUDE.md to defer non-essential items. That table is now **SUSPENDED** for
every item that falls inside the x86_64 v1.0.0 feature set the operator
actually asked for: kernel/backend, userspace, the full AETHER agent roster,
and the full Layer 7 UI/UX design as specified in the project's own design
docs. **Do not silently defer any of the items in section 3 below.** If an
item genuinely cannot ship by the deadline, it must be reported explicitly —
named, with a reason and an estimate — not folded into an exceptions table
and left unmentioned.

The only work still legitimately out of scope for this deadline:
- aarch64 / riscv64 / Apple Silicon full ports (boot-only scope stands)
- Phase 10 (quantum layer) — after v1.0.0 only
- Cloud bridge activation (DDR-793) — explicit security-posture decision, stays deferred
- Rust rewrite — out of scope permanently

Everything else — **all of it** — is in scope for this deadline.

## 3. MANDATORY FULL SCOPE FOR v1.0.0 (x86_64)

This is the complete list. Work through Groups A–H in CLAUDE.md's backlog,
but treat every item below as REQUIRED, not optional, including the ones
previously marked "not started":

### Backend / kernel (Group A, B, C)
- ~~Close B#3 (the CPU-3 AP-liveness freeze, DDR-977) with an actual fix~~
  **DONE — 2026-08-22 23:0x UTC, DDR-981, commit `d7a2912`.** This directive was
  written from the 08-22 state, before the fix landed; recorded here so no
  session re-derives it (which is exactly what §6.2 exists to prevent).
  - **Cause:** `SYSCALL` entry clears `RFLAGS.IF` via `MSR_SFMASK`
    (`syscall.c:229`) and the entry path deliberately never re-enables it
    (`syscall_entry.asm:46`). So every yield-spin reachable from ring 3 spun
    with interrupts MASKED — `mnt_lock`, both pipe waits, the blocking console
    read (PRISM's own read loop), and `sys_yield`. `context_switch` preserves
    per-thread RFLAGS, so the mask is carried ACROSS the switch: two such
    threads on one CPU hand off to each other forever and never reach idle's
    `sti; hlt`. That CPU runs normally with interrupts off — its timer tick
    stops and any block completion MSI-X routed at it is never serviced.
  - **Two framings in the text above are superseded.** It is not "CPU 3" —
    *any* AP freezes, and which one varies with device routing (DDR-977 §8).
    And the CPU is not stalled or halted; it is executing normally, just never
    interrupted.
  - **The instruction not to patch `virtio_blk.c` was correct** — that
    subsystem was behaving correctly throughout. The fix is an interrupt window
    in `yield()` (`sched.c`), the one choke point all five call sites share.
  - **Evidence:** 20/20 boots at `-smp 4`, 0 frozen APs, 0 `compl wait timeout`
    (before: 6/14 boots frozen, 5–11 timeouts each, 0 on every unfrozen boot).
    `ymask` ≈ 6.1M masked yields/boot is the denominator. Mutation-checked.
    `[apfreeze]` added to `GLOBAL_FORBIDDEN` so a recurrence is a named red.
  - **OPEN-2 closed with it** for its block-touching gates (DDR-977 §8.2 chain).
    Not claimed for `smoke-crosswake`/`smoke-msixap`, which do no block I/O.
- Demand-paged stack (ALREADY BUILT — ADR-038, do not rebuild), I/O APIC
  migration, SMEP/SMAP, kernel W^X residual, `#MC` handler, KASLR, scheduler
  timed-block, lock contention instrumentation.
- FAT32/SFS storage completeness items (free-tree persistence, GC, quotas,
  NUMA, ext4 write) — all items in Group B's table. NOTE: "FAT32 multi-cluster
  read fix" is CLOSED as refuted (DDR-973) — do not reopen without a fresh
  `FAT32MC FAIL` artefact from the gated smoke-fat32-multicluster test.
- Networking completeness: UDP, socket poll/select, IPv6, TAP loopback, TLS.

### Userspace / shell (Group D)
- `argv`/`envp` marshalling, PRISM readline/line-discipline, pipes,
  redirection, quoting, job control (`&`, `wait`, `fg`/`bg`), scripting.
- `SYS_MPROTECT`, `SYS_POLL`, `SYS_FUTEX`, pthreads, `mmap` file-backed
  mappings, dynamic linking, full `io_uring`, full POSIX signals.
- NOTE: PRISM `run` is NOT disabled and never was (DDR-973). Only
  init-driven fork+execve RESPAWN of PRISM is unbuilt — build that.

### THE FULL UI/UX DESIGN (Group E) — THIS IS NOT OPTIONAL
This is the part the operator specifically flagged as non-negotiable. Build
every item in Group E's table, not a subset:
- Real modifier keys (F-keys, arrows, Alt, Ctrl, Meta/Super) — not just Tab.
- Super+M physical sovereign-mode toggle.
- Alt-Tab with real modifier plumbing. Ctrl+Alt+T launches a PRISM terminal
  window.
- Per-window restore from dock (not just restore-all).
- Window maximize at REAL display size, not capped at 512×512 (see DDR-975
  sec.7 — the client-side resize-ack handling, not the WM, is the actual bug).
- Pointer resize handles on ALL edges, not just bottom-right.
- `SURF_EV_CLOSE` notifications so an app can save state before forced close.
- The OKLab horizon-band / animated mesh background work from DDR-716.
- The vDSO callable clock reader for ring-3 (`vdso_entry.asm`).
- Fix `smoke-surfdestroy`'s intermittent (OPEN-1).
- NOTE: a flat `preempt=` counter during a compositor capture is NOT evidence
  of a scheduler freeze (DDR-975) — the compositor's yield-loop legitimately
  produces that signature. Do not misdiagnose UI bugs as scheduler bugs.

If the design docs in `docs/design/` specify visual details beyond this list,
build those too — this directive does not narrow the design spec, it removes
permission to skip it.

### The full AETHER agent roster (Group F) — ALL of it
Eleven agents are currently "not started." Build all of them:
`architect_agent`, `healer_agent`, `inventor_agent`, `tournament_agent`, the
subconscious world model, `verifier_agent`, the sovereign NL UI, capability
discovery, lineage memory, the tamper-evident ledger, plus wiring
`CAP_OCR`/`CAP_EXEC`/`CAP_SCENE`/`CAP_NET_BROWSE` so PRAX, LUMYN, AHNIS, and
IRIS are actually spawnable. This is the "backend AI layer" the operator
means when they say "everything we planned" — it is not decoration, it is
core scope for this deadline.

### Release (Group H, x86_64 only for this deadline)
- x86_64 ISO fully working — DONE as of PR #6 (DDR-971/972): the ISO now boots
  a real OS with filesystem, PRISM, AETHER, networking and compositor on BOTH
  BIOS and UEFI paths (DDR-978 fixed UEFI's missing ACPI/PCI discovery too).
  Verify these fixes land on `main` via PR #6's merge.
- `prad` package manager (NSI 88–90).
- Full invariant suite S1–S8, including S3/S7 once F#66–72 land.

## 4. FASTER DEVELOPMENT METHODS — SET THESE UP NOW

The prior pace will not close this scope by 08-28. Change HOW work happens,
not just what gets built:

1. **Parallel branches per group.** Open one scratch branch per Group
   (A through H) and work them concurrently instead of strictly serially.
   Merge each into `dev/phase1-seyp3n` the moment its own gates are green,
   rather than batching everything into one giant PR. Smaller, faster PRs.

2. **Risk-tiered verification, not a flat N=20 for everything.** Scheduler,
   SMP, security, and memory-safety changes keep the N=20 rule — that
   discipline is correct and stays. UI/UX visual items, agent-roster
   additions, and userspace features that don't touch the scheduler,
   capability system, or memory model may use N=5–8 with the same
   falsifiable-denominator discipline, since a compositor color bug and a
   race condition do not carry the same cost of being wrong.

3. **Batch DDR-writing.** For a cluster of related, well-understood items
   (e.g. the eight Group E UI items), write all their DDRs in one pass before
   implementing any of them, so implementation isn't interrupted by
   documentation context-switches.

4. **Widen the CI shard matrix** if the current 6 shards are becoming the
   bottleneck as more gates are added (149 and growing) — this is an
   infrastructure change, do it once, early, rather than repeatedly waiting
   on a saturated matrix.

5. **Report progress in numbers, every session, in `SESSION_HANDOFF.md`:**
   items shipped this session / items remaining in each Group / current
   total against the 286-item tracker. The operator should be able to read
   one file and know exactly where things stand, without needing to ask.

6. **No stopping for confirmation.** Per CLAUDE.md's existing autonomous
   operation rules — those stand. Work continuously through this list. Task
   switch on CI-wait, never idle-stop.

## 5. WHAT "DONE" MEANS NOW

Replace CLAUDE.md's existing "WHAT DONE MEANS" checklist with this one for
this deadline:

- [ ] Every item in section 3 above is built, gated, and CI-green
- [ ] B#3 is FIXED (not just diagnosed — diagnosis is done, DDR-977)
- [ ] PR #6 merged, `dev/phase1` promoted to `main`, 3 greens on the final tip
- [ ] x86_64 ISO manually walked through again on the FINAL tip (not the one
      already verified — verify the actual release candidate)
- [ ] `v1.0.0` tagged on `main`
- [ ] `SESSION_HANDOFF.md` shows 0 remaining items in Groups A–H's x86_64 scope,
      or an explicit, named, reasoned exception — not a silent gap

This is the one chance to get this right. Build everything, report honestly,
and do not let the deadline slip a second time.

---

## 6. AUTOMATION & SPEED PLAYBOOK — specific to what has actually been slow

This section is grounded in this project's OWN build logs, not generic advice.
The recorded slowdowns are: long QEMU boot/verify cycles (90–180s per gate,
30–70 minutes for an N=20 campaign), background measurement processes dying
silently when their wrapper exits (re-running from scratch), re-diagnosing
INTERMITTENTS that were already fully characterized in a prior session, and
CI shard count not growing as gate count grows (149 gates, still 6 shards).
Fix these specifically:

### 6.1 — Fix the background-measurement harness first
Multiple sessions lost 30+ minutes each when `nohup ... &` inside a
backgrounded shell call died the moment its wrapper process exited, silently
restarting a long N-run campaign from run 1 (documented in this project's own
SESSION_HANDOFF history). Before running ANY N=10+ verification campaign:
write a small persistent runner script under `tools/ci/` that:
  - writes its PID and current run number to a status file on every iteration
  - can be polled (`cat build/campaign_status.txt`) without holding a
    foreground shell
  - survives the invoking process exiting (proper `setsid`/`disown`, not bare
    `nohup &` inside a call that itself gets torn down)
This alone reclaims hours already lost twice to the identical failure mode.

### 6.2 — Auto-classify CI failures against the known-signature table
This project already maintains a table of every characterized intermittent
(OPEN-10, `smoke-cadence`, Item 48/B#3, FSRM, etc.) with their exact grep
signatures. Turn that table into a script,
`tools/ci/classify_failure.sh <serial.log>`, that greps a failing log against
every KNOWN signature and prints either "matches <name>, do not re-diagnose,
see <DDR>" or "NEW SIGNATURE — investigate." This directly prevents the
repeated pattern in this project's own history of re-deriving a conclusion a
prior session already reached.

### 6.3 — A fast-lane verification tier, wired into the Makefile
Per section 4.2 of the base directive: add `make smoke-fast N=<gate>
COUNT=<n>` as a real Makefile target (not just a policy statement) that runs
a gate COUNT times and reports pass rate, defaulting to N=5 for anything
tagged non-scheduler/non-SMP/non-security in `tools/ci/gate_shards.txt`. Make
the risk tier a real annotation in that file (a third column), not something
that has to be remembered.

### 6.4 — Widen the CI shard matrix now, not reactively
149 gates on 6 shards is already denser than the 147/6 split that motivated
DDR-817's original sharding work. Rebalance to 8–10 shards using the same
longest-processing-time-first packing DDR-817 already uses
(`tools/ci/gate_shards.txt` duration column) — this is a mechanical, low-risk
change that cuts wall-clock CI time proportionally, and it should happen once,
now, rather than being deferred until the matrix is visibly the bottleneck.

### 6.5 — A generated status dashboard, not manual tallying
Every session currently hand-writes its progress summary into
`SESSION_HANDOFF.md`. Add `tools/ci/status_report.sh` that greps
`docs/BUILD_TRACKER.md` / `docs/AETHER_MASTER_FEATURES.md` for
checked/unchecked items per group and emits a numeric table
(shipped-this-session / remaining-per-group / total-against-286). Run it as
the LAST step of every commit that closes a backlog item, and paste its
output into `SESSION_HANDOFF.md` rather than reconstructing the count by
memory. This is also what should be read to answer "what's the status" —
including from outside a Claude Code session — without needing to re-derive
it from commit history each time.

### 6.6 — Use verification dead-time for the OTHER groups, concretely
The existing rule ("task-switch during CI-wait, never idle-stop") is correct
but abstract. Make it concrete: maintain a literal file,
`docs/NEXT_TASK_QUEUE.md`, as a FIFO of ready-to-start items across Groups
A–H. The moment any long QEMU campaign starts (which blocks further QEMU use
on the same machine), pop the next item off that queue and start its DDR or
code-reading phase — not "whatever seems next," a literal queue that survives
across sessions so nothing is re-picked or forgotten.

### 6.7 — Stop regenerating disk images that have not changed
`fat-image` / `sfs-image` / `esp-image` recipes currently regenerate from
scratch on every gate invocation that depends on them. Where the fixture set
is unchanged between two gate runs in the same session, this is pure
wall-clock cost. Add a content-hash check (hash of the recipe's declared
inputs) so `make fat-image` is a no-op when nothing that feeds it changed —
standard Make dependency tracking, but currently the recipes are `.PHONY` and
always re-run. This is safe because a stale image is caught by the gate's own
assertions failing, not silently accepted.

**None of this changes verification RIGOR** — N=20 stays N=20 where it
matters, every gate still needs a real artefact before a fix ships. This
section only removes time that was being spent on infrastructure friction,
not on the judgment calls this project's discipline exists to protect.
