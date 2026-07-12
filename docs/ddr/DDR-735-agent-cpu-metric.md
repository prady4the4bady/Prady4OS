# DDR-735 — Agent CPU metric: per-thread run-ticks + dispatch count

**Status:** proposed (pre-code)
**Layer:** proc (scheduler accounting) + 6/7 (metrics surface) — live-agent
hardening campaign, slice 2/3.
**Extends:** DDR-730 (per-agent live metrics; its non-goal was exactly this).

## Problem

`SYS_AGENT_METRICS` reports `{pid, state, mem_used, actions}` — nothing about
CPU consumption. An operator watching the agent panel cannot tell a busy agent
from a blocked-forever one, and a runaway spin-looping agent is invisible until
the rate limiter kills it. DDR-730 deferred this because the tcb had no
run-time accounting.

## Decision — two counters, both owner-CPU-written

New `struct tcb` fields (appended at struct end, explicitly initialized in
`sched_create_state` per the tcb-not-zeroed rule):

- **`run_ticks`** — 100 Hz timer ticks observed while this thread was current:
  `sched_tick` increments `current_thread->run_ticks`. Sampled CPU time at tick
  granularity (10 ms) — honest about what it is; fine for "is it busy".
- **`dispatches`** — times the scheduler switched this thread in: `schedule()`
  increments `next->dispatches` at the claim point (`next->state =
  THREAD_RUNNING`). A thread that ever ran has `dispatches >= 1`, which makes
  the gate deterministic even for an agent that lives < 10 ms (run_ticks may
  legitimately be 0 for short-lived threads).

Both counters are written only by the CPU that owns the thread at that moment
(`sched_tick` on the running CPU; `schedule()` under the claim — `on_cpu`
exclusion means no other CPU races the same tcb), so no locking is added to the
hot path. Reads (metrics) are racy-by-a-tick, which is fine for observability.

**Metrics surface:** `struct agent_metric` gains `run_ticks` and `dispatches`
(`{pid, state, mem_used, actions, run_ticks, dispatches}`). The struct is
consumed only by `user/agentmetricstest.c` (kernel-mirrored layout, appended
fields) — the compositor reads the roster bits, unaffected.

## Gate — extend `smoke-agentmetrics` (no new gate; stays 79)

The probe already polls until KRYOS reads live. It now additionally requires
`dispatches >= 1` before declaring success (an agent that is alive AND was ever
scheduled), printing the existing sentinels plus
`AGENT_METRIC KRYOS sched ok`. `run_ticks` is reported, not asserted — a
sub-tick agent lifetime is legal. Extending the existing gate keeps the gate
count stable and tests the same boot flow end-to-end.

## Non-goals

- No TSC-granularity accounting (the 100 Hz tick is the system's time base;
  cycle-accurate accounting is a different, per-switch-cost slice).
- No compositor rendering change (hardening 3/3 decides how to draw it).
- No scheduler-policy use of the counters (observability only).
