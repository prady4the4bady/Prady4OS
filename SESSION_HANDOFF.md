# SESSION_HANDOFF — current state only

Older checkpoints are in `docs/archive/SESSION_HANDOFF-2026-10-17.md`.

## State (2026-11-15)

- **dev/phase1 tip:** `31edffa` (#125, C1.4 P5b: the lockless timer-tick sweep on RCU and the deferred tcb free, DDR-1200 Amendments 2–3). Before it: #126 `debb6a4` (DDR-1203, `map_core` guard design, docs only), #124 `adc4e5c`, #123 `3f9be84` (P5a), #122 `950fc44`, #118 `baac4dd`, #120 `9dba21f`.
- **Kernel:** `kernel.bin` 1,638,794 B against the 1,703,936 B gate, 65,142 B headroom (measured at `baac4dd`; P5b's torture code is test build only, default hash `23ff2ca01b49b863` unchanged). NSI: next free 120. DDR numbers: 1200–1203 in use; **1204 withdrawn** (#127 closed: a duplicate of DDR-1188, the merged C1.5 design); next free is 1205. Before allocating a DDR for a checklist item, search `docs/ddr` and `docs/decisions` for that item's ID first.
- **Work queue (#45 6761846242, the newest owner comment):**
  1. ~~#115~~ #118. 2. ~~#112~~ #120. 3. ~~RCU code~~ P5a #123, P5b #125 (RCU ticked in the checklist).
  4. **Next: C1.5 from DDR-1188** (already designed, merged in #95/#96/#99): P1 the time ABI (`clock_gettime`, `nanosleep` on the existing TSC calibration and vDSO), then P2–P6. Then C1.6 from DDR-1189, C1.7 from DDR-1179 (check the stale KASLR PRs #82, #84, #85 first), then deadlock detection (needs a DDR; none exists).
  - Notes for DDR-1188 from the #127 review: P3's priority inheritance must move a boosted fair owner onto the RT queue and back; P6's tickless idle must wake on RT-throttle budget replenishment.
  - **Merge terms:** the five conditions of 6594914166, with the evidence posted on #45 before the merge. dev/phase1 requires branches to be up to date (strict status checks): merging one PR puts the others behind, and a base merge changes the head, so gated mutants must be re-run on the new head. Merge one gated PR at a time. At most 3 CI runs at once. Post status on #45 at the end of each item, and a full remaining-items list on Nov 16.
- **Small tasks for later (6761846242 item 5: only while CI runs):**
  - **#110:** `execve` keeps the old FS base. Code reading done on #110: exec reuses the caller's tcb and never resets `fs_base` or the MSR. Next: the diagnostic probe (plan step 1). The kernel fix waits for the operator (6655226539).
  - **#108:** the reaper-path gate for the write-back authorization fix (S4-8 survived).
  - The surface logging DDR-1199 proposes, as its own PR.
  - The `map_core` guard: designed in DDR-1203 (#126); next is its code, gate `smoke-mapguard` and mutants M1–M3 (M4, M5 stated not gated).
- **Issues filed this round:** #116 (exit walks), #117 (liveness-gate rebuild), #119 (`vmm_protect_range` address bound wraps). All open, not changed.
- **Known CI defects:**
  - shard 8 `smoke-rqstress` "CPU stops ticking" (DDR-987 §8), seen once on #80; not fixed.
  - A runner DNS outage (`Temporary failure resolving 'azure.archive.ubuntu.com'` in `apt_prepare.sh`) blocked CI from about 13:50 to before 17:30 UTC on Nov 11. Outside the repo.
  - A pull_request run that fails for that reason keeps `mergeable_state` `blocked` even after a green workflow_dispatch run on the same SHA. A fresh pull_request run clears it.
- **Open decision:** the futex `g_ticks` deadline, A/B in #45 6571670160.

## Next

- C1.5 P1 per DDR-1188 §2–§4 (the time ABI, `smoke-schedclass` arms A and B, mutants C1 and C2).
- Then continue from the next unticked item in `docs/V1_MASTER_CHECKLIST.md`. C11.1 stays last.

## Open blockers

- The Dependabot alerts API returns 403 to this session; triage is done with `npm audit`.
- `mkfs.fat` and QEMU are not preinstalled; `apt-get install -y dosfstools mtools qemu-system-x86` lets smoke gates boot locally for screening. CI is the evidence.
- A mutant that only changes `ZANCHOR_TEST` code must be built locally with `ZANCHOR_TEST=1` (`make build/kernel.bin ZANCHOR_TEST=1`). A mutant that only changes `RCU_TORTURE` code must be built with `RCU_TORTURE=1` (`make build/kernel.bin RCU_TORTURE=1`). The default build compiles neither. The flag reaches the compile through `KCFLAGS`, which no target depends on. `build/kernel.bin` depends on the sources, not on the `.o` files, so removing `.o` files does not force a rebuild: touch a kernel source first (the smoke targets touch `kernel/proc/rcu.c` or `kernel/proc/sched.c`) and check the result by hash. Afterwards, touch it again, run `make build/kernel.bin` without the flag, and check the default hash (CodeRabbit on #122 and #124).

## Resume prompt

Read CLAUDE.md, then this file, then issue #45 in full, including owner comments. Follow the newest owner comment first. Continue with "Next".
