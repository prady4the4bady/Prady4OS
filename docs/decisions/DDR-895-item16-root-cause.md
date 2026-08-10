= DDR-895 — item 16 root cause: voluntary yields are never charged

**Status:** Accepted — diagnostic finding. **No scheduler change ships here.**
**Date:** 2026-08-10
**Scope:** `kernel/proc/sched.{c,h}` (observation only), `kernel/main.c` (probe).
**Supersedes the hypothesis in:** the DDR-894 revert report.

## 1. The instrument, and why it does not change behaviour

The pick stays **FIFO**. A shadow `dbg_vruntime` is maintained, the fair-pick
candidate is computed at every `rq_pop`, and the FIFO answer is always the one
returned. Divergences are counted, not acted on.

This matters because DDR-894 was reverted for changing boot-phase progress in a
way its own gate could not see. Measuring a candidate algorithm **by running it**
perturbs the timing under investigation; running it in shadow does not.

Per the added requirement, each thread records `dbg_v_at_create` and
`dbg_v_at_wake` alongside pick order — the two hypotheses are separable only with
those fields.

## 2. The data

`-smp 4`, `QEMU_PROBES=schedtrace`:

```
[schedtrace] picks=512 diverge=993 first_div_tick=89 floor=583680

[schedacct] tid=11 fs   vr=399360 create=24576  wake=578560 picks=48155 ticks=366
[schedacct] tid=87 rqs  vr=583680 create=583680 wake=0      picks=1     ticks=0
[schedacct] tid=86 rqs  vr=583680 create=583680 wake=0      picks=1     ticks=0
... (every rqs thread identical: enters AT the floor, picked once, ~0 ticks)
```

## 3. What it says — neither H1 nor plain H2

**H1 (FS thread starved by accruing vruntime) is REFUTED.** The FS thread's
vruntime is **399360 against a floor of 583680** — it is 184,320 *below* the
floor, not above it. It is the most-favoured thread in the system by fair order,
not the least.

**H2 as stated (probes enter at a stale LOW vruntime) is also REFUTED.** Every
`rqs` thread shows `create == floor` exactly. Entry placement is already correct;
new threads join at the current floor, not at zero.

**The actual mechanism is the inverse of both:**

```
picks=48155   ticks=366
```

The FS thread is picked **48,155 times** but is charged only **366 ticks**. It
yields voluntarily, almost always before the timer tick that would charge it.
`sched_dbg_charge` runs on the tick and charges *the currently running thread*, so
a thread that yields between ticks accrues **nothing**.

Its clock therefore falls arbitrarily far behind a floor driven by everyone else.
Under FIFO this is invisible — round-robin does not read the clock. Under
smallest-vruntime it is a **monopoly**: the FS thread is permanently the minimum,
so it is re-picked every time, while the probe threads that perform the actual
filesystem work — entering at the much higher current floor — are starved.

That is exactly the observed failure set: FS-heavy and long-boot gates fail while
`smoke`, `smoke-rqstress` and the NUMA/UEFI gates pass.

`wake=578560` against `vr=399360` is the same fact from the other side: the
thread has been woken at a high floor many times and its own clock never caught
up, because waking does not charge either.

## 4. The fix direction — for the NEXT slice, not this one

Two mechanisms, both standard CFS and both absent here:

1. **Charge voluntary yields.** Account partial slices at `yield()`/block, not
   only on the timer tick. Without this, any thread that yields faster than the
   tick accrues zero virtual time regardless of how much CPU it consumes.
2. **Clamp on requeue.** Bound how far behind the floor a thread may sit
   (Linux's `place_entity` uses `min_vruntime - threshold`). This is the
   defence-in-depth: even with (1), a thread that blocks for a long time must not
   return with an unbounded credit.

Neither is implemented here. Item 16 remains **OPEN**.

## 5. Instrument limitation, stated

The trace ring **saturates at 512 entries and stops recording**, while
`first_div_tick=89` and `diverge=993` show divergence continues well past that
point. The ring therefore captured only the pre-divergence period and printed no
divergent entries at all — **the per-thread accounting carried this finding, not
the ring.**

A circular overwrite (keep the LAST 512) would have captured the interesting
window. Recorded rather than silently fixed, because the conclusion above rests
entirely on the accounting table and it would be wrong to imply the ring
contributed.
