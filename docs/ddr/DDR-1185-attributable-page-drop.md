# DDR-1185: attributable free-page drop for probe leak arms

Status: **implemented** in PR #90 (smoke-shm arm C), and ported into PR #76 for smoke-thread arm C2. Correct code green and mutants P-M4b, P-INCb and P-TID red in CI on the amended meter (P-M4 and P-INC also red on the first version) (§5). Platform: QEMU TCG only. **Not verified on real hardware.**

## 1. Problem

Two leak arms take their measurement from the system-wide free-page count: `SYS_SYSINFO.free_pages` in smoke-shm arm C, and `SYS_MEMINFO.free` in smoke-thread arm C2. Both run while the rest of the boot is still running (the agent run, service restarts, the Aether daemon), and that traffic moves the count by hundreds of pages in bursts.

Measured:

| Run | Head | Gate | What moved | Result |
|---|---|---|---|---|
| [40210141897](https://github.com/prady4the4bady/Prady4OS/actions/runs/40210141897) | `aceaa04` | smoke-thread C2 | cycle window lost 470 more pages than the idle control | red |
| [40212901781](https://github.com/prady4the4bady/Prady4OS/actions/runs/40212901781) | `3d5f85b` | smoke-thread C2 | same probe; the only change is an unused munmap return value | green |
| [40217768311](https://github.com/prady4the4bady/Prady4OS/actions/runs/40217768311) | `3b904aa` | smoke-thread C | **293 pages lost in a 2 s idle window before the cycles**, then −16 over the cycles and 0 over the idle control | green |
| [40217768311](https://github.com/prady4the4bady/Prady4OS/actions/runs/40217768311) | `3b904aa` | smoke-shm C | private baseline loop drop **−182** (pages returned), shared 0, so shared − private = 182 ≥ 128 | red |

The same smoke-shm arm C was also red once before, in the DDR-1182 M2 run on shard 0 (DDR-1182 §3.2), and was then recorded as "possibly intermittent, not root-caused".

**Mechanism:** the measurement window is not attributable. A single before/after pair counts every other process's allocations and frees as the workload's. A correction by one idle window taken at a *different* time (C2) only cancels the background if that background is steady, and the 293-page idle burst shows it is not. The traffic can push the result either way: it can manufacture a leak (C2 on `aceaa04`, shm C on `3b904aa`) or hide one.

Which processes produce the bursts is **not verified**. The fix does not depend on knowing that, because it rejects contaminated trials instead of modelling them.

## 2. Options

1. **Per-address-space page accounting in the kernel** (an `mm->rss` reported through a syscall). This is exact, but it needs a counter on every user page alloc and free path, plus a new NSI. That is a large change to measure two test arms, and the counter would itself need a gate. Rejected for now.
2. **Wider bounds.** The noise reaches 470 pages, which is above the 384-page effect C2 exists to catch, so a bound wide enough would make C2 blind. That is a weakened assertion. Rejected.
3. **Repeat and take the minimum.** This reduces flakes but does not detect contamination; a burst that returns pages (−182) lowers a minimum just as well. Rejected.
4. **Bracketed trials (chosen).** Run the workload in trials and accept a trial only when the count was provably quiet before it and after it, and no other process appeared or exited across it.

## 3. Design (`user/include/pagemeter.h`)

`pm_measure(work, ctx, &r)` runs up to `PM_TRIALS` (6) trials inside a `PM_BUDGET_TICKS` (30 s) budget. Each trial goes through these steps:

1. **Tid probe.** Fork a child that exits at once and reap it; its pid is `p0`, the tid the kernel handed out (every tcb takes `next_tid++`, `sched.c:1095`; a fork child's pid is its tid, `sched.c:1294`). The probe runs before the quiet wait, so its own pages settle first.
2. **Quiet before.** Wait until `free_pages` holds within `PM_TOL` (4) for `PM_QUIET_TICKS` (25 ticks = 250 ms at 100 Hz). If no quiet window comes before the deadline, the result is inconclusive.
3. **Snapshot.** Take a fingerprint of the scheduler ring (`SYS_GETPROCS`): the count and pid-sum of every entry except the caller and its children. Then sample `f0`.
4. **Run.** Run `work(ctx)`, which returns how many tcbs it created (`own`: 256 forks per shm loop, 64 threads per C2 trial).
5. **Settle.** Settle for `PM_SETTLE_TICKS` (300 ms), so late frees caused by the workload itself (the reaper's tcb and kernel-stack free) land before `f1` is sampled. Then sample `f1` and take the fingerprint again.
6. **Quiet after.** For `PM_QUIET_TICKS`, the count must stay within `PM_TOL` of `f1`. Then a second tid probe gives `p1`.
7. **Accept or reject.** The trial is rejected (`rej_procs`) if the fingerprint changed **or `p1 − p0 − 1 ≠ own`**, that is, if any tcb other than the workload's own was created between the probes, and (`rej_post`) if the post window moved. Otherwise `drop = f0 − f1` is accepted.

No clean trial means `pm_measure` returns 0, and **both callers turn that into a FAIL** (`leaked_ok=0` / `net_leak_ok=0`). Background traffic can therefore make the arm red, never green. Every rejection is counted and printed (`trials`, `rej_pre`, `rej_post`, `rej_procs`), so a red that comes from an inconclusive measurement is distinguishable from a red that comes from a measured leak.

The header is stack-only and issues its own syscalls, so a probe including it carries no writable section (DDR-826).

**Why the tid probe (amended after review on #90).** The first version relied on the fingerprint alone, and its residual-risk note was wrong: a foreign process that starts **and** exits inside a trial is absent from both fingerprints, so it passed. That path is real during boot: init respawns a failing service several times in a few seconds (`[svc] start missing … exit … restart`). A full lifecycle returns most of its pages, but kheap slabs never shrink (`kheap.c` `cache_free` keeps the slab), so slab pages it grows stay out of the PMM. The tid gap catches every tcb creation, however short-lived.

**Deadline.** `pm_quiet` checks the budget before accepting a quiet window, so no trial *starts* after `PM_BUDGET_TICKS`. A trial that started in time and finishes late is still accepted when clean: the budget bounds waiting, and a clean late trial is as attributable as an early one.

**Residual risk:** an already-running process (one that neither starts nor exits) can still allocate or free during a trial. The quiet windows on both sides bound that only for traffic that is ongoing; a burst entirely inside the trial passes. The thread workload is about 20 ms plus a 300 ms settle; the shm loops are longer. Which processes produce the boot traffic is **not verified**.

## 4. Gate changes

**smoke-shm arm C:**
- Each of the private and shared loops is measured by `pm_measure`.
- `leaked_ok = conclusive && (shared − private) < 128`, with the bound unchanged (DDR-1178 §4).
- New ungated lines: `[shm] C meter private|shared trials= rej_pre= rej_post= rej_procs=`, and `[shm] C conclusive=`.

**smoke-thread arm C2:**
- The 64 create/join/munmap cycles are one `pm_measure` workload.
- `net_leak_ok = conclusive && drop ≤ 32`, with the bound unchanged.
- The idle-control subtraction is dropped. A clean trial needs no correction, and subtracting an idle drift could only hide pages. **The arm is stricter than before, not weaker.**

## 5. Mutation check

Two kinds of mutant, landing on different behaviour:

| Mutant | Change | Must be | Why it is not vacuous |
|---|---|---|---|
| P-M4 | DDR-1178 M4 (`vmm_fork_address_space_cow` double `pmm_incref` on `PTE_SW_SHMEM`: one leaked frame per shared cycle) on top of the meter | smoke-shm C `leaked_ok=0`, conclusive=1 | the meter must still see a real leak (≥ 256 pages) |
| P-INC | `pm_quiet` always returns 0 (never quiet) | smoke-shm C `leaked_ok=0`, conclusive=0 (`rej_pre`) | inconclusive must be red; on the unfixed tree the arm has no inconclusive path, so this arm is new |
| P-TID | `loop_run` reports one tcb fewer than it created | smoke-shm C `leaked_ok=0`, conclusive=0, **every trial `rej_procs`** | proves the tid-gap check is wired: correct code passes it, so a check that never rejected would leave this green |

The C2 mutants are recorded in DDR-1181 §4 when the port lands on #76.

**Runs (smoke-shm, shard 0; QEMU TCG).** The first three rows are on `d755b78`, before the tid probe. The probe changes which trials are accepted, so P-M4 and P-INC were re-run on the amended head `56fa48c`, as P-M4b and P-INCb, together with P-TID (the last four rows).

| Tree | Head | Run | Verdict | Measured |
|---|---|---|---|---|
| correct | `d755b78` | [40222132487](https://github.com/prady4the4bady/Prady4OS/actions/runs/40222132487) | green (all 10 shards) | `leaked_ok=1`; drop private=0, shared=0; meter private `trials=2 rej_procs=1`, shared `trials=1`; `conclusive=1` |
| P-M4 | `4dcf948` | [40222095174](https://github.com/prady4the4bady/Prady4OS/actions/runs/40222095174) | red, smoke-shm only | `leaked_ok=0`; drop private=0, **shared=256**; private `trials=2 rej_procs=1`, shared `trials=1`; `conclusive=1` |
| P-INC | `b67bfe3` | [40222096786](https://github.com/prady4the4bady/Prady4OS/actions/runs/40222096786) | red, smoke-shm only | `leaked_ok=0`; both loops `trials=0 rej_pre=1`; **`conclusive=0`** |
| correct | `56fa48c` | [40234192923](https://github.com/prady4the4bady/Prady4OS/actions/runs/40234192923) | green (all 10 shards) | `leaked_ok=1`; drop private=0, shared=0; private `trials=2 rej_procs=1`, shared `trials=1`; `conclusive=1` |
| P-M4b | `e718f04` | [40234278582](https://github.com/prady4the4bady/Prady4OS/actions/runs/40234278582) | red on smoke-shm (shard 0; rest of the run cancelled once it had reported) | `leaked_ok=0`; drop private=0, **shared=256**; private `trials=2 rej_procs=1`, shared `trials=1`; `conclusive=1` |
| P-INCb | `cce7f45` | [40234282490](https://github.com/prady4the4bady/Prady4OS/actions/runs/40234282490) | red, smoke-shm only | `leaked_ok=0`; both loops `trials=0 rej_pre=1`; **`conclusive=0`** |
| P-TID | `4d2abd4` | [40234287018](https://github.com/prady4the4bady/Prady4OS/actions/runs/40234287018) | red, smoke-shm only | `leaked_ok=0`; both loops **`trials=6 rej_procs=6`**; **`conclusive=0`** |

Every mutant is red on the arm it was designed for, and each red is distinguishable from the others by the printed meter line: a measured leak (P-M4 and P-M4b, `conclusive=1`), no quiet window (P-INC and P-INCb, `rej_pre`), and a tid gap that never matches (P-TID, every trial `rej_procs`). Under P-M4 and P-M4b the meter measured exactly the one frame per cycle the mutant leaks (256 over 256 cycles), against 0 on correct code.

**The rejection is real traffic, not noise in the meter.** In every run whose private loop ran on a correctly measured tree (correct and P-M4, before and after the tid probe), that loop's first trial was rejected for `rej_procs`: a process outside the probe started or exited during it. The second trial was clean and measured 0. Under the old single pair, that trial would have been the result. Which process it was is **not verified**.

Mutants were not built locally (there is no QEMU in the container); each was compiled by CI, and every one of these reds is a gate verdict, not a build failure (no compiler errors in the job logs).
