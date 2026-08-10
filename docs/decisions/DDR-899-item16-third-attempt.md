= DDR-899 — item 16, third attempt: the clamp was not the cause

**Status:** Attempt recorded, **reverted**. Item 16 remains OPEN.
**Date:** 2026-08-11
**Follows:** DDR-895 §6.

## What was changed

The DDR-895 §6 defect was real and is fixed in this attempt: `SCHED_LAG_MAX` is
no longer a constant. The clamp is **one measured tick** of vruntime, sampled
from the floor's own advance and smoothed:

```c
d = g_dbg_floor - g_vr_last_tick;
g_vr_per_tick = g_vr_per_tick ? (g_vr_per_tick * 3 + d) / 4 : d;
```

It is therefore correct in whatever units vruntime uses, and a future unit change
cannot silently invalidate it — which is exactly what happened last time.

## It did not fix the failure

| Configuration | `smoke-shell` |
|---|---|
| baseline, item 16 absent (**control**) | **0 / 5 failed** |
| item 16, hard-coded clamp (DDR-895 §6) | 5 / 5 failed |
| item 16, **derived** clamp (this attempt) | **4 / 5 failed** |

The control row is the important one: it was run on the same machine immediately
after the revert, so the attribution is clean. **Fair-share picking breaks
`smoke-shell`, and the clamp magnitude is not the mechanism.**

## What this rules out, and what it does not

**Ruled out:** the unit mismatch (fixed here, failure persists) and the clamp
magnitude generally (now derived from measurement, failure persists).

**Not established:** whether PRISM genuinely processes serial input more slowly
under fair-share, or whether the gate budget is marginal. `smoke-shell` drives
about 25 injected commands with fixed sleep pacing inside a 60 s timeout, so a
guest even slightly slower overruns it.

Those two demand opposite responses. If PRISM is genuinely slower, fair-share is
mishandling a block-on-read workload and the scheduler is wrong. If the gate is
marginal, the scheduler may be fine and the gate needs a budget that is not tuned
to one scheduler's timing. **Raising the timeout now would be tuning the test to
fit the change** — a failure mode this project has repeatedly paid for — so it
was not done.

## The measurement that would separate them

Instrument the shell, not the scheduler: timestamp block-to-wake-to-dispatch for
PRISM console reads and compare the distribution with and without fair-share.

- Per-byte latency unchanged, only wall-clock total moved → the gate is marginal.
- Block-to-dispatch latency grows → the scheduler is at fault and the wake path
  needs the work.

That is a fourth diagnostic cycle, and it is the honest next step rather than
guessing at a third constant.

## Status

Item 16 is **OPEN**. Three attempts; two genuine defects found and fixed along
the way (tick-only charging, DDR-895; the unit mismatch, here). The remaining
failure is not yet attributed. 17/18 gates is not shipping.

---

## Addendum — the separating experiment was run, and it answered

Rather than a fourth instrumented cycle, one cheap experiment separates the two
hypotheses directly: raise **only** the `smoke-shell` QEMU timeout (mirror only,
never committed) and see whether fair-share then passes.

- Budget-marginal → passes with more time.
- PRISM genuinely stalled → fails regardless.

| Configuration | Result |
|---|---|
| fair-share, stock 60 s | 3 / 3 FAIL |
| fair-share, **200 s** | **3 / 3 FAIL** |

**The gate budget is NOT the cause.** With 3.3x the time, `kill %99` still never
executes and the failure lands at the identical point. This is not "PRISM is
slower"; PRISM **stops making progress**.

This is not test tuning: the timeout change was discarded either way, and its
only purpose was to distinguish two hypotheses demanding opposite fixes.

### What that means

Fair-share picking **stalls an I/O-blocked interactive thread**. PRISM blocks in
`SYS_READ` on the console, is woken by the serial IRQ, and after `jobs: none`
never visibly runs again — even across 200 seconds.

The wake path is now the prime suspect, not the pick order:

- `sched_place()` runs from `rq_push`, which is reached via `sched_unblock` →
  requeue. If a woken thread's `dbg_vruntime` is *above* the floor, `sched_place`
  does nothing (it only lifts threads that are behind). A thread that ran a long
  slice before blocking therefore returns **above** the floor and stays behind
  every fresh thread entering at it.
- The clamp is one-sided by construction, and one-sidedness is correct for
  anti-starvation but leaves no mechanism for **sleeper credit** — the thing that
  makes an interactive shell responsive.

That is a concrete, testable next hypothesis and it names a specific line. It is
not another constant to guess.

### Status

Item 16 remains **OPEN**, now with the budget hypothesis eliminated by
experiment and the failure localised to the wake/requeue path.
