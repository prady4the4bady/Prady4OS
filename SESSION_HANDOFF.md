# SESSION_HANDOFF — current state only

Older checkpoints are in `docs/archive/SESSION_HANDOFF-2026-10-17.md`.

## State (2026-11-18)

- **dev/phase1 tip:** `1c4e6bc` (#138, C1.5 P3: `SCHED_FIFO`/`SCHED_RR`, the RT throttle, `RES_SCHED`, one-level `kmutex` PI; DDR-1188 Amendment 4; arms D, E, F, G, H, N, P). Before it: `2506c94` (#137, the DDR-1203 `map_core` guard, `smoke-mapguard`, `GLOBAL_FORBIDDEN` 78), `e19ca88` (#132, bookkeeping), `5086ec6` (#136, C1.5 P2: one-shot LAPIC timers, DDR-1188 Amendment 3; arms C and M). Before those: `0f16c16` (#133, DDR-1206 and Amendment 1: `FUTEX_WAIT` ends on a handlerless SIGTERM/SIGPIPE, but not inside an active handler; arms FK, FL). Before it: #134 `03a3326` (DDR-1178 §A4.10, arm FR, the reaper-path write-back gate; closed #108), #131 `b2181de` (DDR-1205, `execve` resets the FS base; closed #110), #130 `ec6187d` (C1.5 P1).
- **Kernel:** `kernel.bin` 1,683,850 B against the 1,703,936 B gate, 20,086 B headroom (printed by CI on `45e8319`, run 44217157028). P4–P6 must fit in that. NSI: 122 `SYS_SCHED_SETSCHED`, 123 `SYS_SCHED_GRANT`; next free 124. DDR numbers: next free is 1207. Before allocating a DDR for a checklist item, search `docs/ddr` and `docs/decisions` for that item's ID first.
- **Work queue (#45 6761846242, the newest owner comment):**
  1. ~~#115~~ #118. 2. ~~#112~~ #120. 3. ~~RCU code~~ #123, #125.
  4. **C1.5 from DDR-1188:** ~~P1~~ #130, ~~P2~~ #136, ~~P3~~ #138. **P4 is next** (`SCHED_DEADLINE`; needs a DDR-1188 amendment before code). Then P5, P6. Then C1.6 from DDR-1189, C1.7 from DDR-1179 (check the stale KASLR PRs #82, #84, #85 first), then deadlock detection (needs a DDR; none exists).
  - Notes for DDR-1188 (A1-5): P3's priority inheritance must move a boosted fair owner onto the RT queue and back; P6's tickless idle must wake on RT-throttle budget replenishment.
  - **Merge terms:** the five conditions of 6594914166, with the evidence posted on #45 before the merge. dev/phase1 requires branches to be up to date (strict status checks): merging one PR puts the others behind, and a base merge changes the head, so gated mutants must be re-run on the new head. Merge one gated PR at a time. At most 3 CI runs at once. Post status on #45 at the end of each item.
- **Open PRs:**
  - This bookkeeping PR (#137 and #138 in the checklist and here).
  - Review threads left open for the operator: `docs/build_status.md` on #136 and #138 (question in #45 6948599917).
- **Issues filed:** #135 (one local `PTHR_R` "R create" failure in 7 runs; mechanism not named; the probe prints no return code). Earlier, all open: #116, #117, #119.
- **Follow-ups recorded, not started:** the four other waits that ignore a handlerless SIGTERM (DDR-1206 §4); `signal_deliver` acting on default-terminate signals while a handler is active (DDR-1206 A1).
- **Lessons:**
  - Push work as soon as it builds. A container replacement lost an uncommitted build once.
  - Never `git checkout <file>` to drop a mutant from a tree with uncommitted work; revert the mutant with its own script.
  - A probe's design assumption about kernel state (DDR-1203's slot 256) must be checked by running it before the gate is trusted.
  - A probe arm that a mutant can damage must restore what it snapshots whatever the call returned, or one mutant's damage reaches the control arm.
- **Small tasks still open (6761846242 item 5):** the surface logging DDR-1199 proposes, as its own PR.
- **Known CI defects:**
  - shard 8 `smoke-rqstress` "CPU stops ticking" (DDR-987 §8), seen once on #80; not fixed.
  - A pull_request run that fails for a runner outage keeps `mergeable_state` `blocked` even after a green workflow_dispatch run on the same SHA. A fresh pull_request run clears it.
  - The `github-advanced-security` check fails with "exceeded your monthly quota" (#134); not a required check.
- **Open decision:** the futex `g_ticks` deadline, A/B in #45 6571670160.

## Next

- C1.5 P4 (`SCHED_DEADLINE`): DDR-1188 amendment first, then code, arms and mutants. Size headroom is 20,086 B.
- Then continue from the next unticked item in `docs/V1_MASTER_CHECKLIST.md`. C11.1 stays last.

## Open blockers

- The Dependabot alerts API returns 403 to this session; triage is done with `npm audit`.
- `mkfs.fat` and QEMU are not preinstalled; `apt-get install -y dosfstools mtools qemu-system-x86` lets smoke gates boot locally for screening. CI is the evidence.
- A mutant that only changes `ZANCHOR_TEST` code must be built locally with `ZANCHOR_TEST=1` (`make build/kernel.bin ZANCHOR_TEST=1`). A mutant that only changes `RCU_TORTURE` code must be built with `RCU_TORTURE=1` (`make build/kernel.bin RCU_TORTURE=1`). The default build compiles neither. The flag reaches the compile through `KCFLAGS`, which no target depends on. `build/kernel.bin` depends on the sources, not on the `.o` files, so removing `.o` files does not force a rebuild: touch a kernel source first (the smoke targets touch `kernel/proc/rcu.c` or `kernel/proc/sched.c`) and check the result by hash. Afterwards, touch it again, run `make build/kernel.bin` without the flag, and check the default hash (CodeRabbit on #122 and #124).

## Resume prompt

Read CLAUDE.md, then this file, then issue #45 in full, including owner comments. Follow the newest owner comment first. Continue with "Next".
