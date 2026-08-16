= DDR-924 — bound the off-CPU spin: fall back to idle instead of burning the CPU

**Status:** ACCEPTED.
**Date:** 2026-08-15
**Lineage:** DDR-919 (freeze fix) → DDR-922 (measurement) → **DDR-924 (this)**.

## The measurement that forces this

DDR-922 instrumented `switch_wait_offcpu_sched` and drained the counter per
500-tick heartbeat window. Measured, `-smp 4`:

```
[hb] t=1500 spins=3106850 max=1314074 cpu=1
[hb] t=2000 spins=2075036 max=1036945 cpu=1
[hb] t=3500 spins=2246234 max=1096888 cpu=0
```

2–3 million spins per 5 s window; ~1.3 million on one CPU. DDR-922's stated
threshold for "the suppression is real" was >50 per window.

Two things follow:

1. `g_in_switch` is set for a large fraction of wall time, so `sched_tick` skips
   its `schedule()` call correspondingly often — preemption is **suppressed**,
   not deferred. That matches every post-DDR-919 CI failure's shape ("did not
   complete in time").
2. `switch_wait_offcpu`'s comment — *"Bounded: the holder is executing a few
   instructions, never blocked on us"* — is **false**. A CPU does not run 1.3
   million `pause` iterations waiting for a few instructions.

## What the fix may NOT be

**Not** "clear `g_in_switch` after N spins and keep spinning with IF=1."
`sched.c:983-989` documents the governing invariant:

> `schedule()` must be atomic against the timer … If a voluntary `schedule()`
> ran with interrupts enabled … a timer tick could re-enter `schedule()`
> mid-context-switch and corrupt thread state. Masking interrupts here makes it
> non-reentrant.

DDR-919 kept that invariant's *intent* while changing its *mechanism*: interrupts
are briefly enabled so the tick can advance `g_ticks`, and `g_in_switch`
suppresses the reentrant `schedule()` that would otherwise follow. Dropping the
flag mid-spin re-opens precisely the reentrancy the comment warns about. So the
bound cannot be "stop suppressing and carry on spinning".

**Not** "abandon the wait and use `next` anyway." `next`'s `rsp` is not yet
saved; that is the whole reason the wait exists (rq-2 D3).

## Decision

Bound the spin, and on expiry **give up on this pick** rather than on the
invariant:

1. Spin at most `SWITCH_WAIT_MAX_SPINS` iterations.
2. If `next` is still on-CPU, put it **back on the runqueue** and switch to this
   CPU's **idle** thread instead. Idle is never enqueued and never runs on
   another CPU, so it is always immediately available to its own CPU — no second
   wait is possible.
3. `g_in_switch` stays set for the whole (now bounded) window, so the
   non-reentrancy invariant holds unchanged.

Forward progress is preserved: the contended thread stays runnable and is picked
again on a later tick, by whichever CPU can actually take it. The CPU that would
have burned a million `pause` iterations instead runs idle and — crucially —
becomes preemptible again immediately.

### The bound is derived, not chosen

The wait exists to cover "the holder is executing a few instructions" (rq-2 D3):
`context_switch` saving `rsp`, then `finish_task_switch`'s release store. That is
tens of instructions, i.e. well under a microsecond even on TCG. A bound of
**4096** iterations is ~3 orders of magnitude above the intended case, so the
legitimate wait never trips it, while the pathological case (10^6) is cut by
~250x. It is a backstop for a violated invariant, not a tuning parameter.

## What this does NOT fix

It treats the symptom. **A release that takes a million pauses is a scheduler
defect in its own right** — something is holding `on_cpu` far longer than rq-2's
design allows, and this DDR does not explain what. That investigation is
separate and is the more important one; this change stops one CPU from
monopolising itself while it proceeds.

The `[hb] spins=` instrument from DDR-922 is retained precisely to show whether
the bound is being hit, and how often.

## Gate

`smoke-blkmq`, `smoke-rqstress-liveness` and `smoke-blk-integrity` are the
regression surface (scheduler-sensitive, and two of them are the former freeze
sites). Build warning-clean, three `ci-*-check` PASS. The `spins=`/`max=` values
after the change are themselves the verification: `max` must fall to at most the
bound.

## RESULT — bound works per call, but does NOT solve the aggregate. Correction below.

Measured after the change, `-smp 4`, same 45 s boot:

```
[hb] t=1000 spins=1442280 max=520372 cpu=3
[hb] t=2500 spins=1784680 max=634302 cpu=3
[hb] t=3500 spins=1670892 max=591528 cpu=3
```

### Correction to this DDR's own verification criterion

The Gate section above said "`max` must fall to at most the bound". **That was
wrong, and the measurement shows why.** `max` is the *cumulative* spin count for
one CPU across a whole 500-tick window, not the largest single call. The bound
caps each *call* at 4096; it says nothing about how many calls occur. Observed
`max` ≈ 590k–634k with a 4096 bound implies roughly **150+ bounded calls per
CPU per window**.

So the correct reading is:

- **Per call: fixed.** No single `switch_wait_offcpu_sched` can now burn ~10^6
  iterations with preemption suppressed; the worst case is 4096.
- **In aggregate: barely improved.** Total spins fell only ~2–3M → 1.4–1.8M.
  The CPU is still spending a large fraction of its time in this path.

### What that actually proves

The problem is **not** that one wait is long. It is that `schedule()` is being
entered constantly and *almost always finds `next` still on-CPU elsewhere*. That
is scheduler thrashing, and it is the real defect — the one DDR-922 flagged as
"a release that takes a million pauses is a scheduler defect in its own right".
Bounding the spin is a guard against pathological CPU monopolisation, and it is
worth keeping on that merit, but it must not be recorded as having fixed the
underlying behaviour. It has not.

### Next investigation (not started)

Instrument the *call* rate and the bail rate separately from the spin count:
how many times per window is `switch_wait_offcpu_sched` entered, and how many of
those hit the bound? A high entry count with a high bail ratio says CPUs are
fighting over the same small set of runnable threads — which would point at the
rq steal/pop policy, not at the wait itself.

### Regression surface — clean

`smoke-blkmq`, `smoke-rqstress-liveness`, `smoke-blk-integrity` all rc=0 after
the change (the latter two are the former freeze sites).
