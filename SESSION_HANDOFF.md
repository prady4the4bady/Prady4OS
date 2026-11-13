# SESSION_HANDOFF — current state only

Older checkpoints are in `docs/archive/SESSION_HANDOFF-2026-10-17.md`.

## State (2026-11-12)

- **dev/phase1 tip:** `3f9be84` (#123, C1.4 P5a: the kernel RCU grace-period core, DDR-1200 Amendment 1). Before it: #122 `950fc44` (bookkeeping), #118 `baac4dd` (DDR-1201, #115: the tick sweep starts on the ring), #120 `9dba21f` (DDR-1202, #112: `mprotect` copies a shared private page before adding write), #114 `97f0397` (DDR-1200, RCU design), #111 `2a508d5`.
- **Kernel:** `kernel.bin` 1,638,794 B against the 1,703,936 B gate, 65,142 B headroom (measured at `baac4dd`). NSI: `SYS_FUTEX` 117, `SYS_SCHED_CPU` 118, `SYS_MSYNC` 119; next free 120. DDR-1200, 1201 and 1202 are in use; next free is 1203.
- **Work queue (#45 6761846242, the newest owner comment):**
  1. ~~#115~~ merged as #118. 2. ~~#112~~ merged as #120.
  3. **RCU code (DDR-1200), in two parts (Amendment 1).** P5a merged as #123 (arms RA/RB/RC, U1–U3 red). **Next: P5b**, the lockless `idle0`-anchored sweep with the act step under the waiter's `block_lk`, and `rcu_defer_free` with the reaper drain; arms RD/RE/RF, mutants U4–U9. Record any survivor as not gated.
  4. Then C1.5, C1.6, C1.7 (check #84 and #85 first), then deadlock detection, each with a DDR first.
  - **Merge terms:** the five conditions of 6594914166, with the evidence posted on #45 before the merge. Stop and report on #45 if anything is red or inconclusive, if the shard 8 failure recurs, or if CodeRabbit shows the design is wrong. At most 3 CI runs at once. Post status on #45 at the end of each item, and a full remaining-items list on Nov 16.
- **Small tasks for later (6761846242 item 5: only while CI runs):**
  - **#110:** `execve` keeps the old FS base. Code reading done on #110: exec reuses the caller's tcb and never resets `fs_base` or the MSR. Next: the diagnostic probe (plan step 1). The kernel fix waits for the operator (6655226539).
  - **#108:** the reaper-path gate for the write-back authorization fix (S4-8 survived).
  - The surface logging DDR-1199 proposes, as its own PR.
  - A DDR, gate and mutant for the `map_core` guard proposed in DDR-1198.
- **Issues filed this round:** #116 (exit walks), #117 (liveness-gate rebuild), #119 (`vmm_protect_range` address bound wraps). All open, not changed.
- **Known CI defects:**
  - shard 8 `smoke-rqstress` "CPU stops ticking" (DDR-987 §8), seen once on #80; not fixed.
  - A runner DNS outage (`Temporary failure resolving 'azure.archive.ubuntu.com'` in `apt_prepare.sh`) blocked CI from about 13:50 to before 17:30 UTC on Nov 11. Outside the repo.
  - A pull_request run that fails for that reason keeps `mergeable_state` `blocked` even after a green workflow_dispatch run on the same SHA. A fresh pull_request run clears it.
- **Open decision:** the futex `g_ticks` deadline, A/B in #45 6571670160.

## Next

- C1.4 P5b per DDR-1200 §5, §5.1–§5.2 and D5 (6761846242 item 3).
- Then continue from the next unticked item in `docs/V1_MASTER_CHECKLIST.md`. C11.1 stays last.

## Open blockers

- The Dependabot alerts API returns 403 to this session; triage is done with `npm audit`.
- This container has no `mkfs.fat`, so smoke gates cannot boot locally; CI is the evidence.
- A mutant that only changes `ZANCHOR_TEST` or `RCU_TORTURE` code must be built locally with that flag (`make build/kernel.bin ZANCHOR_TEST=1`); the default build does not compile it. The flag reaches the compile through `KCFLAGS`, which no target depends on, so remove the `.o` files first and check the result by hash (CLAUDE.md, Hard-won facts). Afterwards, remove the `.o` files again, run `make build/kernel.bin` without the flag, and check the default hash (CodeRabbit on #122).

## Resume prompt

Read CLAUDE.md, then this file, then issue #45 in full, including owner comments. Follow the newest owner comment first. Continue with "Next".
