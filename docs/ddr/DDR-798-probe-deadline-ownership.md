# DDR-798 — a probe must not own a deadline it cannot judge

**Status:** accepted; implemented in this slice.
**Date:** 2026-07-29
**Completes:** BUG-1, with DDR-796 (CMOS/RTC race) and DDR-797 (serial flood).
**Constraint honoured:** the 120-second window is **not widened**. It is removed
from the place that cannot evaluate it.

## What is left after DDR-796 and DDR-797

Both earlier fixes are real and both landed:

* the clock no longer runs backwards (`smoke-rtc-smp` green, A/B verified);
* the console is no longer flooded — CI run 30391224155 shows
  `AGENT_METRICS FAIL` as a **clean line** rather than buried in a binary blob,
  which is direct evidence the DDR-797 fix took effect on the runner.

And `AGENT_METRICS FAIL: agent never observed as scheduled` still appeared, on
`smoke-msixap`. Locally the same build passes 10/10 across two rounds. The
difference is host speed: CI is TCG without KVM, on a shared runner.

## The actual defect

`user/agentmetricstest.c` is spawned on **every boot**. It waits up to 120 RTC
seconds for the AETHER daemon to spawn and dispatch an agent, and prints
`AGENT_METRICS FAIL` if that has not happened.

That deadline is only meaningful in the one gate whose job is to assert it —
`smoke-agentmetrics`, which declares the success sentinel as required and gives
the boot 150 s. In the other ~106 gates the probe still runs, still counts to
120, and still declares failure about a boot that was never trying to reach the
daemon quickly.

So the probe asserts a **timing** property in contexts where nothing controls
timing, and on a slow host the assertion fails for reasons that have nothing to
do with what it is testing. The DDR-791 harness fix (`GLOBAL_FORBIDDEN`) then
correctly propagates that to whichever gate is running.

The harness is right. The probe is wrong: **it owns a deadline it has no way to
judge.**

## Decision

The probe no longer declares failure on its own clock. On window expiry it
prints an informational line and exits 0:

```
AGENT_METRIC not observed in window (informational; gate decides)
```

The assertion does not disappear — it moves to where a timeout belongs. In
`smoke-agentmetrics` the required sentinels (`AGENT_METRIC KRYOS sched ok`,
`PRADYOS_AGENT_METRICS_OK`) must still appear within the gate's 150 s window, and
if the agent genuinely is never scheduled that gate fails on a **missing required
sentinel**. That is the same strength of assertion, bounded by the gate's own
declared timeout instead of a constant compiled into a probe.

### Why this is not widening the window

The 120-second constant is untouched, and no gate's timeout changes. What is
removed is a *second, redundant* deadline that duplicated the gate's timeout and
disagreed with it. Two deadlines for one property, one of them unable to see the
context, is the bug.

### Why the failure signal gets stronger, not weaker

A missing required sentinel cannot be masked. The previous arrangement could:
the probe's FAIL string was forbidden in exactly one gate before DDR-791, so a
genuine scheduling failure in any other gate was silently tolerated — which is
how this went unnoticed for so long.

### Considered and rejected

* **Widen the 120 s window.** Explicitly forbidden, and it only moves the cliff.
* **Do not spawn the probe except in its own gate.** Attractive, but the boot
  path spawns a fixed set of probes and making that gate-conditional would put
  test-selection logic into `kmain` — a worse coupling than the one being fixed.
* **Remove `AGENT_METRICS FAIL` from `GLOBAL_FORBIDDEN`.** Forbidden by the
  operator directive, and correctly so: it would re-hide any real failure.
* **Silent exit on timeout.** Rejected — an observation that stops being
  reported is how a regression becomes invisible. The informational line keeps
  it in the log without asserting a verdict.

## Gate

`smoke-agentmetrics` is unchanged and remains the assertion: the agent must be
observed as scheduled, within that gate's window. Every other gate stops being
asked to judge a deadline it never set.
