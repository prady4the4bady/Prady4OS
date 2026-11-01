# SESSION_HANDOFF — current state only

Older checkpoints are in `docs/archive/SESSION_HANDOFF-2026-10-17.md`.

## State (2026-11-01)

- **dev/phase1 tip:** `f0e1bb6` (PR #91, C1.4 P1d, DDR-1186). Before it: #97 `757983b` (DDR-1191, CI pinned to `ubuntu-24.04`), #76 `3358b35` (C1.4 P1c), #77 `cd3281a` (DDR-1183 size gate). Branch protection is on; the operator merges.
- **Kernel:** `kernel.bin` 1,577,354 B with #81 merged onto `f0e1bb6` (local build, 0 warnings); gate 1,703,936 B. NSI: `SYS_FUTEX` 117 (#81), `SYS_SCHED_CPU` 118 (#91). DDR next free: 1193.
- **DDR-1177 C1.4 arm D, arm E and M4 ticked** with `f0e1bb6` (T4, T5, T6 red on `a4a508a`). Emulated only. Cross-CPU TLB coherence for threads is not verified on real hardware.
- **#81 (C1.4 P2 futex, security boundary):** `dev/phase1` merged in as `547e1c5` (5 textual conflicts, both sides kept). Needs one green run on that head and F2-1 to F2-8 red again before it asks for merge.
- **#98 (DDR-1192):** updated onto `f0e1bb6`; carries this bookkeeping. One green run, then the operator merges. The CodeRabbit marker-list finding was answered and withdrawn (thread 4431625714).
- **Then:** #78 → #80 (C1.3 P2/P3, security boundary). Also open: #88, #72, #85, #90.
- `dev/phase1` `2a49a73` `smoke-iso-userspace` surface-lifecycle failure is still unexplained; DDR-1192 keeps its evidence if it repeats.

## Next

- Merge-driven bookkeeping as PRs land; fix any PR a merge leaves conflicted.
- Then continue from the next unticked item in `docs/V1_MASTER_CHECKLIST.md`. C11.1 stays last.

## Open blockers

- The Dependabot alerts API returns 403 to this session; triage is done with `npm audit`.
- This container has no `mkfs.fat`, so smoke gates cannot boot locally; CI is the evidence.

## Resume prompt

Read CLAUDE.md, then this file, then issue #45 in full, including owner comments. Follow the newest owner comment first. Continue with "Next".
