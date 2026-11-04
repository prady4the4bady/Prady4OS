# SESSION_HANDOFF — current state only

Older checkpoints are in `docs/archive/SESSION_HANDOFF-2026-10-17.md`.

## State (2026-11-04)

- **dev/phase1 tip:** `8d69135` (#103, C1.3 P4 shared file mappings, head `5f8e31a`, run 41748812981). Before it: #80 `9105001`, #78 `287ae93`, #102 `ca53351`, #104 `a70bfd4`. **C1.3 is done.**
- **Kernel:** gate 1,703,936 B. NSI: `SYS_FUTEX` 117, `SYS_SCHED_CPU` 118, `SYS_MSYNC` 119. DDR-1197 is reserved for C1.4; 1198 (#105) and 1199 (#107) are in use; next free is 1200.
- **Merge authority:** operator 6618228303 lets the session merge #105, and the user also authorised #106, each once green on its exact head with no unanswered CodeRabbit finding.
- **This PR (#105, DDR-1198):** the `vmm_map_in` caller audit, docs only. It also carries this bookkeeping.
- **Open follow-ups:**
  - #106: DDR-1196 Amendment 1, W1's panic cause.
  - #107: DDR-1199, the surface failures.
  - #108: the reaper-path gate.
  - A DDR, gate and mutant for the `map_core` guard proposed in DDR-1198.
- **Next item:** C1.4, DDR-1197 first, once #103 and #105 have merged.
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
