= DDR-904 — item 16 SHIPPED: CFS fair-share + AI hint lane

**Status:** Accepted. Item 16 **CLOSED**.
**Date:** 2026-08-11
**Closes:** DDR-894, DDR-895, DDR-899, DDR-902 (five refuted hypotheses).

## The cause: placement was never running

Hypothesis 6, measured directly during a failing `smoke-shell` boot:

```
[place] rate=0 calls=1     zero=1
[place] rate=0 calls=1025  zero=1025
[place] rate=0 calls=2049  zero=2049
[place] rate=0 calls=3073  zero=3073
[place] rate=0 calls=4097  zero=4097
[place] rate=5089 calls=6145 zero=4097 lift=0 credit=0
```

`g_vr_per_tick` was **0 for the first 4,097 `sched_place()` calls**, so the early
return fired every time and **no placement happened at all** during the entire
early-boot window.

That single fact explains every previous null result. Guard 1 (elapsed-time
charging), guard 2 (the clamp), the derived clamp, and the two-sided sleeper
credit all live *behind* that return. They were correct and they were never
reached — which is exactly why each measured zero effect and why four
increasingly careful fixes changed nothing.

The rate was sampled as an **instantaneous floor delta between two timer ticks**.
On a largely idle single-CPU system — which is what `smoke-shell` becomes once
boot finishes — that delta is frequently zero, so the rate never established.

## The fix

Seed the rate from the **first real charge**:

```c
if (!g_vr_per_tick && inc)
    g_vr_per_tick = inc;
```

Non-zero as soon as any thread has run at all, and in the correct units **by
construction**, because it *is* a charge. The per-tick sampler then refines it.
This is the third form of the same lesson: the DDR-895 §6 constant was wrong
because it was hand-picked in the wrong unit; this one cannot be, because it is
never picked.

## A Heisenbug, caught before it was believed

The instrumented build passed **0/5** with the diagnostics in place. That was not
the fix. `sched_place()` runs under the runqueue lock with interrupts off, and
`kputs` there perturbs precisely the timing under test.

The diagnostics were removed and the run repeated: **still 0/5**. Only then was
the fix real. Had the instrumented pass been taken at face value, a build whose
correctness depended on serial-output timing would have shipped.

## Results

| Configuration | `smoke-shell` |
|---|---|
| baseline, item 16 absent (control) | 0 / 5 |
| one-sided clamp (DDR-895 §6) | 5 / 5 FAIL |
| derived clamp (DDR-899) | 4 / 5 FAIL |
| two-sided credit (DDR-899 addendum) | 5 / 5 FAIL |
| **rate seeded, diagnostics removed** | **0 / 5** |

Full 18-gate regression, uncontended, **all green** — including all nine gates
this item previously broke: `smoke-fs`, `smoke-fs-sfs-rw`, `smoke-fs-ext4`,
`smoke-user`, `smoke-shell`, `smoke-sfs-btree`, `smoke-sfs-persist`,
`smoke-e1000e`, `smoke-ahci`.

## What ships

- **CFS fair-share pick** — smallest `vruntime` rather than FIFO head, with a
  linear scan rather than a red-black tree (the per-CPU queue is short by
  construction; the tree is a measurement-gated optimisation).
- **Elapsed-time charging** via TSC, so a thread yielding between ticks pays for
  what it used. The original defect: 48,155 picks against 366 ticks charged.
- **Two-sided placement** — anti-starvation lift, plus sleeper credit granted
  only on wake, never on preemption requeue, so a CPU-bound thread cannot launder
  away its cost by bouncing through the queue.
- **AI hint lane** — `sched_ai_hint()` clamped to 4x on use as well as on set.
  An unbounded hint is a starvation primitive.

## Honest notes

`credit=0` in the trace: the sleeper-credit branch never fired in that run,
because after `sched_charge_elapsed` at block time a thread is usually already at
or below the floor. The branch is correct and bounded, but it is **not exercised**
by the current gates and should not be claimed as proven.

Six hypotheses were required. Five were refuted by measurement, three genuine
defects were found and fixed along the way, and the sixth was confirmed by one
counter that should have been added four attempts earlier. The lesson worth
keeping: **every one of those fixes was correct and unreachable.** Verifying that
a code path executes at all belongs before verifying what it does.
