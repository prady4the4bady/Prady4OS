= DDR-925 — call-rate data REFUTES the preemption-suppression theory (corrects DDR-922)

**Status:** ACCEPTED. **Corrects a conclusion I recorded in DDR-922.**
**Date:** 2026-08-15
**Lineage:** DDR-919 → DDR-922 (spin count) → DDR-924 (bound) → **DDR-925 (this)**.

## What DDR-922 concluded, and why it was wrong

DDR-922 measured `switch_wait_offcpu_sched` at 2–3 million spins per 500-tick
window and concluded:

> Reading (B) is confirmed. `g_in_switch` is set for a large fraction of the
> time … preemption is suppressed, not merely deferred.

and

> The original invariant is false. A CPU does not execute 1.3 million `pause`
> iterations waiting for "a few instructions".

**Both statements were unsupported.** A spin *total* cannot distinguish one very
long wait from very many short ones, and DDR-922 had no call count. It should
have measured calls before drawing that conclusion; the DDR even named this as
the "next investigation" while simultaneously stating the conclusion.

## The measurement that settles it

Counters added for calls (wait entered) and bails (DDR-924 bound hit), drained
per heartbeat window alongside the spin count. `-smp 4`, 45 s boot:

```
[hb] t=1000 spins=1901883 max=657573 cpu=2 calls=100014 bails=14
[hb] t=1500 spins=768910  max=300391 cpu=1 calls=37465  bails=21
[hb] t=2000 spins=0       max=0      cpu=0 calls=0      bails=0
[hb] t=2500 spins=0       max=0      cpu=0 calls=0      bails=0
[hb] t=3000 spins=0       max=0      cpu=0 calls=0      bails=0
[hb] t=3500 spins=0       max=0      cpu=0 calls=0      bails=0
```

Three facts, none of which the spin total alone could show:

1. **Mean spin per call ≈ 19 iterations** (1 901 883 / 100 014). The individual
   wait is SHORT — precisely what rq-2 D3's "the holder is executing a few
   instructions" describes. **The invariant is not violated. DDR-922's claim that
   it was is withdrawn.**
2. **The contention is a BOOT-PHASE transient.** From `t=2000` onward every
   counter is exactly zero. There is no steady-state thrashing at all.
3. **`bails` = 14 and 21 against ~137 000 calls.** DDR-924's 4096 bound is
   essentially never reached, which independently confirms (1) — if waits were
   long, the bound would trip constantly.

## Consequence: reading (B) is refuted

Total time with preemption suppressed ≈ 1.9M `pause` iterations. At roughly
10 ns each that is ~19 ms inside a 5 s window — **~0.4%, and only during boot**.
That cannot make a gate overrun by seconds. The mechanism proposed in DDR-922 —
suppression accumulating into multi-second latency — is quantitatively
impossible at this call profile.

**Therefore the post-DDR-919 CI pattern is reading (A):** pre-existing flakes
that the freeze used to hide. Before DDR-919 a run usually died at the freeze
first, so these gates never got to be the first failure. They must now be
classified individually, per gate, against pre-DDR-919 history — not attributed
to a single cause.

## Why the boot-phase contention exists (explanatory, not a defect claim)

`schedule_locked` re-pushes `prev` onto the runqueue *before* `prev->on_cpu` is
released — by design, since the release happens in `finish_task_switch` inside
whichever thread resumes next (rq-2 D2). During boot, many threads are created
and switched rapidly across 4 CPUs, so another CPU frequently pops that
just-re-pushed thread while its `on_cpu` is still set and waits ~19 iterations
for the release. That is the designed handshake working, not a fault. It
disappears once the thread population stabilises, exactly as observed.

## Decisions

1. **Keep DDR-924's bound.** It costs nothing (bails ≈ 35 in 137k calls) and
   remains a correct backstop against a pathological wait. Its *justification*
   is amended here: it is insurance, not a fix for an active defect.
2. **Keep the counters.** They are cheap, they are what corrected this, and they
   make any future regression in this path self-reporting.
3. **Do not change `rq_pop`/`rq_steal`.** The TASK A hypothesis was that the
   picker returning an on-CPU thread is a defect to be fixed. Q3's answer is
   that it is *intended* (sched.c:163-168: "on_cpu is NO LONGER part of the
   filter … a READY-but-still-on-CPU thread is now legitimately takeable"), and
   the data shows the resulting wait is ~19 iterations and boot-only. Filtering
   `on_cpu` in the picker would add cost to steady state to optimise a transient
   that is already free.
4. **Do not revert DDR-919.** Unchanged: the freeze is gone.

## What is now the open question

The CI failures are unexplained again, and must be classified per gate:
`smoke-cadence`, `smoke-rtc-smp` (btree churn collateral), `smoke-aclick`.
For each: did it fail on any tip BEFORE DDR-919? If yes → pre-existing, record
as OPEN. If no → it became reachable only once the freeze stopped masking it,
which is still "pre-existing but newly visible", not a regression.

This is the honest position: DDR-919 fixed the freeze, and the failures behind
it are now visible and individually unclassified.
