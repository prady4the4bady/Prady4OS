= DDR-890 — measure the DDR-887 spin, and fix the stray-QEMU guard that cannot fail

**Status:** ACCEPTED (instrument + tooling correction). **No behavioural kernel
change.**
**Date:** 2026-08-15
**Lineage:** DDR-887 (freeze fix) → **DDR-890 (this)**.

## Part 1 — the discriminating instrument

Since DDR-887 landed, four consecutive CI runs each failed on a *different*
single shard, and all three named failures share one shape — "did not complete
in time":

```
smoke-cadence   [cadence] FAIL — no full auto cycle
smoke-rtc-smp   [sfs] churn FAIL op=create iter=0 rc=-1   (collateral, DDR-785)
smoke-aclick    [aclick] FAIL — the clicked PRAX agent did not run to completion
```

Two readings are open and neither is settled:

- **(A)** pre-existing flakes that the freeze used to hide — before DDR-887 a run
  usually died at the freeze first, so these never got to be the first failure.
- **(B)** a timing regression introduced *by* DDR-887: `sched_tick` skips its
  `schedule()` call while `g_in_switch` is set, so preemption is suppressed for
  the duration of a `switch_wait_offcpu_sched` spin. If that spin is frequent,
  preemption gaps accumulate and latency-sensitive gates run long.

The two are indistinguishable by argument and trivially distinguishable by
measurement, so this DDR measures instead of arguing.

`switch_wait_offcpu_sched` now counts its spins per CPU, and the existing
heartbeat drains and prints them once per 500-tick window:

```
[hb] t=<ticks> thre_drops=<n> rx_drops=<n> spins=<total> max=<n> cpu=<id>
```

`spins` is the total across CPUs for that window; `max`/`cpu` name the busiest.
The counter is `__atomic_exchange_n`-drained so each line describes exactly one
window with no double-counting.

**Reading the result:**

- `spins` consistently low (single digits per window) ⇒ the suppression window
  is too rare to explain multi-second gate overruns. **(B) refuted by data**, and
  the pattern is (A) — pre-existing flakes to be classified individually.
- `max` high (>~50 per window on any CPU) ⇒ the suppression is real. The fix is
  then to **bound** the spin: after N iterations, stop suppressing preemption and
  let the scheduler run normally. That is a separate DDR, written only once the
  data justifies it.

This costs one relaxed atomic add per spin iteration — on a path that, if the
measurement comes back low, essentially never runs.

## Part 2 — the stray-QEMU guard has never worked

The standing rule is "`pgrep qemu-system-x86_64` must return empty before any
gate run". **That check cannot fail.** Linux truncates `comm` to 15 characters
and `qemu-system-x86_64` is 18, so name-matching pgrep returns zero matches
whether or not QEMU is running — it even says so:

```
pgrep: pattern that searches for process name longer than 15 characters
       will result in zero matches
```

So every "no stray QEMU" observation recorded against that command was vacuous.
Given that concurrent QEMU on this host has already produced two retracted root
causes, a guard that silently always passes is worse than none.

`pgrep -f` matches the full command line and does work — but it also matches the
*invoking shell*, whose command line contains the pattern, producing a false
positive. Observed exactly that while running the hygiene set for this commit.

**Correct form**, using the bracket trick so the pattern cannot match itself:

```sh
pgrep -f "[q]emu-system-x86_64"
```

This is documented here rather than silently used, because the old command
appears throughout the session history and its results should be treated as
uninformative, not as evidence of a clean host.

## Gate

No new gate. This is instrumentation plus a documentation/tooling correction;
the existing timing-sensitive gates are the regression surface. Build must stay
warning-clean and `smoke-blkmq` rc=0.

## RESULT — measured locally, reading (B) CONFIRMED

`-smp 4`, 45 s boot, heartbeat drained per 500-tick window:

```
[hb] t=1500 thre_drops=0 rx_drops=0 spins=3106850 max=1314074 cpu=1
[hb] t=2000 thre_drops=0 rx_drops=0 spins=2075036 max=1036945 cpu=1
[hb] t=2500 thre_drops=0 rx_drops=0 spins=2134803 max=1075472 cpu=1
[hb] t=3000 thre_drops=0 rx_drops=0 spins=2153723 max=1097821 cpu=1
[hb] t=3500 thre_drops=0 rx_drops=0 spins=2246234 max=1096888 cpu=0
```

**2–3 million spins per 5 s window; ~1.1–1.3 million on a single CPU.** The
threshold stated above for "the suppression is real" was >50 per window. The
observation exceeds it by four orders of magnitude.

Conclusions, in order of confidence:

1. **Reading (B) is confirmed.** `g_in_switch` is set for a large fraction of
   the time, so `sched_tick` skips its `schedule()` call correspondingly often
   and preemption is suppressed, not merely deferred. That is sufficient to make
   latency-sensitive gates run long, which is the shape of every post-DDR-887
   failure.
2. **The original invariant is false.** `switch_wait_offcpu`'s comment claims
   "Bounded: the holder is executing a few instructions, never blocked on us."
   A CPU does not execute 1.3 million `pause` iterations waiting for "a few
   instructions". The holder is *not* being released promptly.
3. **DDR-887 is still correct and must not be reverted.** The freeze is gone —
   heartbeats stream, and both freeze gates pass. What this shows is that the
   `sti` made the system *live* while leaving it *thrashing*: before the fix the
   same contention deadlocked silently; now it spins visibly. The spin was
   always there; only its lethality changed.

### Consequence

The spin must be bounded: after a bounded number of iterations, stop suppressing
preemption and let the scheduler run normally rather than burning the CPU. That
is a behavioural change to the scheduler and gets its own DDR with its own
gate — it is not folded in here.

Separately, the *reason* the holder takes so long to release `on_cpu` is now the
real question. Bounding the spin treats the symptom; it stops one CPU from
monopolising itself, but a release that takes a million pauses is a scheduler
defect in its own right.
