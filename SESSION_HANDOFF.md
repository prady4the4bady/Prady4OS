# SESSION_HANDOFF — current state only

Older checkpoints are in `docs/archive/SESSION_HANDOFF-2026-10-17.md`.

## State (2026-11-07)

- **dev/phase1 tip:** `2a508d5` (#111, C1.4 P4b `pthr.h`, head `5bab41c`, run 42353336050; mutants R1–R8 red). Before it: #109 `6bb8062` (DDR-1197), #107 `71ecbb6`, #106 `62eeb75`, #105 `d62e7ea`, #103 `8d69135`. **C1.3 is done; C1.4 pthreads is ticked.**
- **Kernel:** unchanged by #111. Gate 1,703,936 B. NSI: `SYS_FUTEX` 117, `SYS_SCHED_CPU` 118, `SYS_MSYNC` 119. DDR-1197, 1198 and 1199 are in use; next free is 1200.
- **Merge authority:**
  - Docs-only PRs (6647074754): merge once green on the exact head, `mergeable_state` is clean and CodeRabbit has nothing unanswered.
  - Code PRs: the five conditions of 6594914166.
  - Anything touching the kernel, a syscall, capabilities or page tables waits for the operator (6655226539).
- **Open follow-ups:**
  - **#112:** kernel isolation defect, by reading. A read-only private page shared at fork can be made writable by `mprotect`. Waits for the operator (kernel change).
  - **#110:** `execve` may keep the old FS base (DDR-1197 §10); a probe is allowed, any kernel fix waits.
  - **#108:** the reaper-path gate.
  - The surface logging DDR-1199 proposes, its own PR.
  - A DDR, gate and mutant for the `map_core` guard proposed in DDR-1198.
- **Known CI defect:** shard 8 `smoke-rqstress` "CPU stops ticking" (DDR-987 §8), seen once on #80; not fixed.
- **Open decision:** the futex `g_ticks` deadline, A/B in #45 6571670160.

## Next

- Merge-driven bookkeeping as PRs land; fix any PR a merge leaves conflicted.
- Then continue from the next unticked item in `docs/V1_MASTER_CHECKLIST.md`. C11.1 stays last.

## Open blockers

- The Dependabot alerts API returns 403 to this session; triage is done with `npm audit`.
- This container has no `mkfs.fat`, so smoke gates cannot boot locally; CI is the evidence.

## Resume prompt

Read CLAUDE.md, then this file, then issue #45 in full, including owner comments. Follow the newest owner comment first. Continue with "Next".
