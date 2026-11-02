# SESSION_HANDOFF — current state only

Older checkpoints are in `docs/archive/SESSION_HANDOFF-2026-10-17.md`.

## State (2026-11-02)

- **dev/phase1 tip:** `76bfbc6` (PR #100, DDR-1194: `smoke-part` lines in one `kline` write; mechanism fixed, not proven by a gate). Before it: #81 `cc99745` (C1.4 P2 futex), #99 `4909109` (DDR-1193 design; D1: priority inheritance moves to C1.5 P3), #98 `65357d3`. Branch protection is on; the operator merges.
- **Kernel:** gate 1,703,936 B. NSI: `SYS_FUTEX` 117, `SYS_SCHED_CPU` 118. DDR next free: 1195.
- **This PR (#101, C1.4 P3a):**
  - `user/include/psync.h` and `smoke-thread` arms SA, SW, SB, SC, SH, SD (DDR-1193 Amendment 1).
  - Green on `2bd99ca`, and Y1–Y6 each red on its own arm (Amendment 1 runs).
  - SA catches Y1 only probabilistically (a margin of one update). MTTCG is not verified.
  - When it merges, tick the C1.4 "mutex, semaphore, condition variable, barrier" row with the merge SHA.
- **Then:** #78 → #80 (C1.3 P2/P3, security boundary). Also open: #88, #72, #85, #90.
- **Follow-ups owed from #81:**
  - the stale lock comment at `sched.c:880-883`;
  - FF/FG need a blocked-waiter witness (CodeRabbit thread 4435326775);
  - the futex timeout gets +1 tick when `ticks > 0`, with FC at 51 ms and a new mutant.
- **Unexplained, not fixed:**
  - `SURFDESTROY FAIL: reuse slot not reused` (run 41367275758, shard 7, `smoke-numa-steal`); queued as a separate task.
  - `dev/phase1` `2a49a73` `smoke-iso-userspace` surface lifecycle; DDR-1192 keeps its evidence.

## Next

- Merge-driven bookkeeping as PRs land; fix any PR a merge leaves conflicted.
- Then continue from the next unticked item in `docs/V1_MASTER_CHECKLIST.md`. C11.1 stays last.

## Open blockers

- The Dependabot alerts API returns 403 to this session; triage is done with `npm audit`.
- This container has no `mkfs.fat`, so smoke gates cannot boot locally; CI is the evidence.

## Resume prompt

Read CLAUDE.md, then this file, then issue #45 in full, including owner comments. Follow the newest owner comment first. Continue with "Next".
