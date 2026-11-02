# SESSION_HANDOFF — current state only

Older checkpoints are in `docs/archive/SESSION_HANDOFF-2026-10-17.md`.

## State (2026-11-01)

- **dev/phase1 tip:** `cc99745` (PR #81, C1.4 P2 futex, DDR-1181 Amendment 1). Before it: #99 `4909109` (DDR-1193, C1.4 P3 design; D1 approved: priority inheritance moves to C1.5 P3), #98 `65357d3` (DDR-1192), #91 `f0e1bb6` (C1.4 P1d). Branch protection is on; the operator merges.
- **Kernel:** `kernel.bin` 1,577,354 B on `cc99745` (local build, 0 warnings); gate 1,703,936 B. NSI: `SYS_FUTEX` 117, `SYS_SCHED_CPU` 118. DDR next free: 1195.
- **C1.4 futex ticked** with `cc99745`: arms FA–FJ green, F2-1 to F2-8 red on `547e1c5`. The merged head `5997083` passed on run 41299374169 attempt 2; attempt 1 failed `smoke-part` on a console line split by another CPU. QEMU only.
- **This PR (DDR-1194):** prints each `[part]` line as one `kline`, which removes the splice race behind that failure. Mechanism fixed, not proven by a gate. It also carries this bookkeeping and the DDR-1181 A1.3 SIGTERM/SIGPIPE limit.
- **Then:** P3a (DDR-1193 §5: `psync.h`, six `smoke-thread` arms, mutants Y1–Y6; report early whether QEMU runs the vCPUs in parallel). Then #78 → #80 (C1.3 P2/P3, security boundary). Also open: #88, #72, #85, #90.
- **Follow-ups owed from #81:** the stale lock comment at `sched.c:880-883`; FF/FG need a blocked-waiter witness (CodeRabbit thread 4435326775); futex timeout +1 tick when `ticks > 0`, with FC at 51 ms and a new mutant.
- `dev/phase1` `2a49a73` `smoke-iso-userspace` surface-lifecycle failure is still unexplained; DDR-1192 keeps its evidence if it repeats.

## Next

- Merge-driven bookkeeping as PRs land; fix any PR a merge leaves conflicted.
- Then continue from the next unticked item in `docs/V1_MASTER_CHECKLIST.md`. C11.1 stays last.

## Open blockers

- The Dependabot alerts API returns 403 to this session; triage is done with `npm audit`.
- This container has no `mkfs.fat`, so smoke gates cannot boot locally; CI is the evidence.

## Resume prompt

Read CLAUDE.md, then this file, then issue #45 in full, including owner comments. Follow the newest owner comment first. Continue with "Next".
