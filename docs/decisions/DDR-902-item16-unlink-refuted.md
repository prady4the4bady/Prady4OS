= DDR-902 — item 16: the rq_unlink suspect is REFUTED by measurement

**Status:** Hypothesis refuted. No fix written. Item 16 remains OPEN.
**Date:** 2026-08-11
**Follows:** DDR-899 addendum.

## The suspect

DDR-899 identified, by inspection, that `rq_unlink()` clears `rq_on` and unlinks
the thread **before** testing `THREAD_READY`, returns 0 when that test fails, and
the caller does not fall back to `rq_take()`:

```c
__atomic_store_n(&c->rq_on, 0, __ATOMIC_RELEASE);
return (c->state == THREAD_READY) ? c : 0;
```

If that path ever fired, the thread would be unlinked, marked un-queued, and
returned to nobody — **lost from every runqueue**. A lost thread reads exactly
like the observed PRISM stall: blocked forever, not slow.

## Instrumented before fixing, and it does not fire

Four counters were added and the code left otherwise unchanged — the suspect was
**measured, not corrected**, because the three previous hypotheses all looked
equally convincing before measurement refuted them.

```
[unlink] calls=746753 notfound=0 notready_LOST=0 pop_empty_with_queue=0
```

- **746,753** fair-path unlinks.
- **0** candidates missing from the queue.
- **0** unlinked-then-rejected — the loss path never executes.
- **0** occasions where `rq_pop` returned nothing while the queue was non-empty.

**Refuted.** The defect is real as written and should still be repaired for
robustness when item 16 eventually lands, but it is **not** the mechanism behind
the `smoke-shell` stall.

## Hypotheses eliminated so far

| # | Hypothesis | How it died |
|---|---|---|
| 1 | FS thread starved by accrued vruntime | measured below the floor, not above |
| 2 | probe threads enter at a stale low vruntime | `create == floor` exactly |
| 3 | gate budget marginal | 3/3 fail at 200 s, same failure point |
| 4 | one-sided clamp starves wakers | two-sided credit: still 5/5 fail |
| 5 | `rq_unlink` loses threads | 0 occurrences in 746,753 calls |

Three genuine defects were found and fixed along the way (tick-only charging,
the clamp unit mismatch, the one-sided clamp). None was the cause.

## Next suspect, stated as a hypothesis and NOT acted on

`sched_place()` returns early when `g_vr_per_tick == 0`:

```c
uint64_t lag = g_vr_per_tick;
if (!lag)
    return;                /* not yet measured */
```

`g_vr_per_tick` is sampled from the floor's advance **per timer tick**. On a
largely idle single-CPU system — which is exactly what `smoke-shell` is once boot
completes — the floor barely moves, so the sampled delta is frequently zero and
the rate may never establish. If it stays 0, **placement never happens at all**:
no anti-starvation lift, and no sleeper credit. PRISM would then keep whatever
vruntime it accrued while running and sit behind the idle thread permanently.

This would also explain why the two-sided fix changed nothing: both branches live
behind that early return.

**The measurement that settles it:** print `g_vr_per_tick` from the shell gate's
own boot, not from the `-smp 4` trace boot. Non-zero refutes it; zero or rarely
sampled confirms it. That is one counter and one run.

Per instruction, no fifth fix is attempted before that measurement exists.
