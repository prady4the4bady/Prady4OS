# SESSION_HANDOFF — current state only

Older checkpoints are in `docs/archive/SESSION_HANDOFF-2026-10-17.md`.

## State (2026-11-05)

- **dev/phase1 tip:** `71ecbb6` (#107, DDR-1199 surface-failures record, docs only, head `b4641e9`, run 41830893956). Before it: #106 `62eeb75` (DDR-1196 Amendment 1), #105 `d62e7ea` (DDR-1198), #103 `8d69135` (C1.3 P4), #80 `9105001`, #78 `287ae93`, #102 `ca53351`, #104 `a70bfd4`. **C1.3 is done.**
- **Kernel:** gate 1,703,936 B. NSI: `SYS_FUTEX` 117, `SYS_SCHED_CPU` 118, `SYS_MSYNC` 119. DDR-1197 (this PR), 1198 and 1199 are in use; next free is 1200.
- **Merge authority (6647074754):** the session may merge any **docs-only** PR once it is green on its exact head, `mergeable_state` is clean and CodeRabbit has nothing unanswered. A PR that touches code, a gate, a Makefile or a workflow is not docs only: code PRs follow the five conditions of 6594914166, and a security boundary waits for the operator. When blocked on a question with a clear safe option, take it, say so on #45, and keep working.
- **This PR (DDR-1197):** the C1.4 P4 design, a native pthreads library `pthr.h` on `psync.h`, docs only. It also carries the bookkeeping for #107.
- **Open follow-ups:**
  - The surface logging DDR-1199 proposes (print `a` and `b` on failure), its own PR (6647074754 item 5).
  - #108: the reaper-path gate.
  - A DDR, gate and mutant for the `map_core` guard proposed in DDR-1198.
  - DDR-1197 §10: `execve` may keep the old FS base (not verified); its own DDR.
- **Next item:** C1.4 P4b (`pthr.h`, `pthrtest`, `smoke-pthread`, R1–R8 red), after DDR-1197 merges.
- **Known CI defect:** shard 8 `smoke-rqstress` "CPU stops ticking" (DDR-987 §8), seen once on #80; not fixed.
- **Open decision:** the futex `g_ticks` deadline, A/B in #45 6571670160.
- **Unexplained, not fixed:** both surface failures (DDR-1199).

## Next

- Merge-driven bookkeeping as PRs land; fix any PR a merge leaves conflicted.
- Then continue from the next unticked item in `docs/V1_MASTER_CHECKLIST.md`. C11.1 stays last.

## Open blockers

- The Dependabot alerts API returns 403 to this session; triage is done with `npm audit`.
- This container has no `mkfs.fat`, so smoke gates cannot boot locally; CI is the evidence.

## Resume prompt

Read CLAUDE.md, then this file, then issue #45 in full, including owner comments. Follow the newest owner comment first. Continue with "Next".
