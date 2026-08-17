= DDR-947 — A2 CAUGHT: the runqueue stops draining on a present, ticking CPU

> **STATUS CORRECTED — read "CORRECTION" at the bottom FIRST. The central claim
> of this document (that the 42-pin is the A2 signature) is WRONG; it came from
> a single run. Four further A2-class failures show a HEALTHY scheduler.**

**Status:** ~~ACCEPTED~~ **REFUTED as an A2 signature.** One 42-pin event
observed, mechanism unknown; A2's mechanism remains **open**.
**No fix in this slice.**
**Date:** 2026-08-16
**Evidence:** local `smoke-agent-click`, tip `2a20001` + DDR-946 instruments,
run 3 of 12 (`build/gatelogs/ac-FAIL-3.log`). Confirmed **A2** by DDR-946's
discriminator: triggered pids 50 and 55 appear in **no** `sys_exit` line.
**Numbering:** 947 verified free in both `docs/ddr/` and `docs/decisions/`
(§0.4). 945 and 946 taken.

## The measurement DDR-942 was built for

`rqdepth` series across the 21 heartbeats of the failing boot:

```
2, 1, 2, 2, 2, 6, 7, 11, 16, 32, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42
└─ baseline ─┘  └─ monotonic climb ─┘  └── pinned at 42 for 11 heartbeats ──┘
```

Every other counter, on all 21 heartbeats:

```
ubcas=0 ubrq=0 ubst=0 rqmiss=0 rqmst=0   rqq=1 rqpres=1   spins=0 max=0
```

A **passing** boot sits flat at `rqdepth=6` (DDR-942 baseline).

This satisfies DDR-942's stated criterion exactly — "depth **above baseline**
and **monotonically non-decreasing**, read as a series" — and it is the first
time that criterion has fired. The baseline was established *before* this
reading, so the criterion was not fitted to the data.

## What it means, and what it excludes

**42 threads are enqueued on CPU 0 and are never picked, for ~55 s, while the
CPU is present and the timer keeps firing.** The heartbeats themselves prove
the timer ISR is running throughout.

| excluded | by |
|---|---|
| enqueue failed (CAS gate) | `ubcas=0` |
| enqueue skipped (`rq_on` gate) | `ubrq=0` |
| dropped at pick time | `rqmiss=0` |
| stranded on a non-present CPU (DDR-944) | `rqq=1 rqpres=1` — CPU 0 is present |
| lock convoy in `switch_wait_offcpu` | `spins=0 max=0` |
| crash / assertion | zero `[BUG]` lines |
| allocation | threads exist and are queued |

So the threads are **queued, live, on a running CPU, and simply not selected**.
The defect is in the *pick*, or in whatever is preventing a switch away from the
current thread.

## Where it stops

The last non-heartbeat line in the boot is:

```
[sfs] freelist persist OK
```

After that: 55 s of heartbeats and **nothing else**. So a single thread is stuck
**without yielding**, and everything spawned after it accumulates behind it on
the uniprocessor.

Crucially it is *not* blocked: a blocked thread leaves the runqueue and the
others get picked. `rqdepth` pinning at 42 means **no** context switch is
completing. The timer ISR fires (heartbeats), but preemption is not switching
away from the stuck thread.

This is the same shape as the ITEM 47 note in CLAUDE.md §0.2 — "reaching stamp A
and never stamp C … it BLOCKS, it does not crash" — but the runqueue evidence
sharpens it: whatever it is doing, it is doing it **without leaving the CPU**.

## Hypotheses, explicitly NOT adopted

1. The stuck thread runs with interrupts disabled except during the ISR, so
   `sched_tick`'s preemption path never takes effect on it.
2. `sched_tick` reaches the preempt point but `g_in_switch` (DDR-887) suppresses
   the reschedule indefinitely rather than for one window.
3. The thread is in a kernel loop that never calls `schedule()` and is not
   preemptible at its current point.

Each fits. **That is exactly why none is adopted here.** Eleven mechanisms have
been retired in this investigation, every one plausible, every one wrong, and
every retirement made by an instrument rather than an argument.

## Next instrument (not a fix)

Identify the thread occupying the CPU while `rqdepth` is pinned. The heartbeat
already prints `cpu=`; add the **current thread's pid/name** and a
**preempt-attempt counter** from `sched_tick`:

- `curpid=`/`curname=` — names the stuck thread directly.
- `preempt=` — counts `sched_tick` preempt attempts; if it climbs while
  `rqdepth` is pinned, hypothesis 2/3 (attempted and suppressed); if it stays
  flat, hypothesis 1 (the tick path is not reaching the preempt point for that
  thread).

Two fields, both on the existing `[hb]` line, no new sentinel.

**Do not touch `sti;pause;cli` or `g_in_switch` (DDR-887, CLAUDE.md §0.1 — DO
NOT REVERT) until `preempt=` has spoken.**

## What would refute this DDR

- `rqdepth` pinning on a **passing** run too (then the pin is normal and the
  criterion is wrong — but the passing baseline of 6 flat argues against it).
- The pinned entries turning out to be unrunnable (all non-READY), which
  `rqmiss=0` already argues against, since `rq_take` would have dropped them.

## Scope (§6.0-C)

This is **A2 only**. A1 (agent runs, exits 0, prints nothing — DDR-945) has a
different signature and keeps its own root cause. The blk workers (`done=0x0`)
and rqstress (`n=8/24 spawned=24/24`) are kernel threads in other subsystems and
are not merged here either — though the "queue stops draining" signature is now
a *candidate* common cause for them, to be tested, not assumed.

---

## CORRECTION — the 42-pin is NOT the A2 signature (same day, 14-run hunt)

Ran 14 more with the `preempt=`/`supp=`/`cur=` counters live. **9 failed.**
Genuine mode-A failures (a trigger fired, second assertion) read:

```
rqdepth: 2 2 2 2 2 7 7 6 6 6 6 6 6 6 6 6 6 6 6      <- baseline, flat
preempt: 1700 1701 1702 1703 1704 1705               <- climbing every heartbeat
supp:    0 0 0 0 0 0                                  <- never suppressed
cur:     COMPOSIT.ELF -> reaper -> SURFTEST.ELF -> COMPOSIT.ELF
```

**The scheduler is healthy in these A2 failures.** `rqdepth` sits at baseline,
`preempt` climbs every heartbeat, `supp` stays 0, and `cur` rotates across
different threads — context switches are happening throughout.

**So this DDR's central claim is wrong.** `rqdepth=42` pinned for 11 heartbeats
happened on **one** run (`ac-FAIL-3`) and is not what A2 looks like generally.
I generalised a signature from a single sample — the same error as DDR-945's
over-broad "mode A" claim, two DDRs earlier.

The 42-pin remains a real, separately-observed event worth its own
investigation; it is **not** the A2 mechanism and must not be cited as such.

`preempt` climbing with `supp=0` and the queue draining also means the three
hypotheses this DDR listed (interrupts disabled, `g_in_switch` suppression,
non-preemptible loop) are **all refuted** for these failures. Twelve mechanisms
retired.

## A defect in the harness, not the kernel

The A2 classifier had a false-positive path:

```sh
a2=1
for p in $(… triggered pids …); do
    if grep -q "sys_exit… pid=$p"; then a2=0; fi
done
```

With **zero** triggers the loop body never runs and `a2` stays 1, so a mode-B
failure (no trigger at all) is reported as A2. Two of the four "A2" runs
(`a7-FAIL-3`, `a7-FAIL-13`) had `trig=0` and were mode B. Any classifier must
first assert `trig>0`, then test the exit. Recorded because a mis-classified
sample is worse than no sample — it is what produced this DDR's wrong claim.

## The instrument may be perturbing the measurement

Failure rate went **2/12 (~17%) → 9/14 (~64%)** after adding `cur=` (a string
`kputs`) to the timer ISR. That is a large jump and the ISR print is the only
behavioural change; two relaxed atomics cannot account for it.

This is the hazard flagged for `BTN_STATE` in DDR-941 and not heeded here: an
instrument heavy enough to shift timing changes the thing it measures. **The
9/14 rate must not be quoted as this build's flake rate**, and `cur=` should be
made conditional (print only when `rqdepth` exceeds baseline) before any further
rate comparison.

## Status

**Downgraded to: one observed 42-pin event, mechanism unknown; A2's mechanism
still open.** No fix. No mechanism adopted.
