# DDR-1185: attributable free-page drop for probe leak arms

Status: **implemented** on branch `fix-pagemeter` (smoke-shm arm C), and ported into PR #76 for smoke-thread arm C2. CI evidence is in §5. Platform: QEMU TCG only. **Not verified on real hardware.**

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

1. **Quiet before.** Wait until `free_pages` holds within `PM_TOL` (4) for `PM_QUIET_TICKS` (25 ticks = 250 ms at 100 Hz). If no quiet window comes before the deadline, the result is inconclusive.
2. **Snapshot.** Take a fingerprint of the scheduler ring (`SYS_GETPROCS`): the count and pid-sum of every entry except the caller and its children. Then sample `f0`.
3. **Run.** Run `work(ctx)`.
4. **Settle.** Settle for `PM_SETTLE_TICKS` (300 ms), so late frees the workload itself causes (the reaper's tcb and kernel-stack free) land before `f1` is sampled. Then sample `f1` and take the fingerprint again.
5. **Quiet after.** For `PM_QUIET_TICKS`, the count must stay within `PM_TOL` of `f1`.
6. **Accept or reject.** The trial is rejected if the fingerprint changed or the post window moved. Otherwise `drop = f0 − f1` is accepted.

No clean trial means `pm_measure` returns 0, and **both callers turn that into a FAIL** (`leaked_ok=0` / `net_leak_ok=0`). Background traffic can therefore make the arm red, never green. Every rejection is counted and printed (`trials`, `rej_pre`, `rej_post`, `rej_procs`), so a red that comes from an inconclusive measurement is distinguishable from a red that comes from a measured leak.

The header is stack-only and issues its own syscalls, so a probe including it carries no writable section (DDR-826).

**Residual risk:** a burst that both starts and ends inside one trial, with no process start or exit, passes the brackets. The thread workload is about 20 ms plus a 300 ms settle, so its exposure is short. The shm loops are longer. The fingerprint closes the case the shm failure most likely was (a process lifecycle overlapping the loop; attribution not verified).

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
| P-INC | `pm_quiet` always returns 0 (never quiet) | smoke-shm C `leaked_ok=0`, conclusive=0 | inconclusive must be red; on the unfixed tree the arm has no inconclusive path, so this arm is new |

The C2 mutants are recorded in DDR-1181 §4 when the port lands on #76.

Runs: *(filled in when CI reports; until then **not verified**.)*
