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

**Retention (so the gate is deterministic).** A short-lived agent's tcb vanishes
the instant it exits, and `state >= 1` (alive) only overlaps `dispatches >= 1`
during its brief CPU time — asserting both in the same sample is racy. So the
roster slot RETAINS the last-read `run_ticks`/`dispatches` (refreshed from the
live tcb on every metrics read, kept after exit); the probe then latches the two
facts INDEPENDENTLY across samples — (a) ever seen alive while slot 7 stayed
idle, (b) dispatches ever >= 1 (readable post-exit) — and passes on both. This
keeps the "alive" catch as wide as the DDR-730 gate while adding the CPU proof.

## Gate — extend `smoke-agentmetrics` (no new gate; stays 79)

**CI post-mortem (run 29203329840) — the alive-window assertion was racy.** On
CI's TCG runners a compositor quantum takes seconds, so the test agent's whole
life (spawn -> DONE, visible in the serial) fit between two probe samples: the
`state >= 1` latch never fired, and since retention was refresh-ON-READ, the
counts stayed zero too — the probe spun silently past the gate timeout. (Local
KVM samples thousands of times during the agent's life, which is why it passed
here.) Two corrections, both making the proof **post-mortem stable**:

1. **Exit capture**: `sched_exit` calls `agent_metrics_reap(pid, run_ticks,
   dispatches)` (the DDR-729 reap-hook pattern), so the roster's retained
   counters are authoritative regardless of read timing; the dead slot also
   retains its **pid** (identity checkable after death; state stays 0).
2. **Probe/gate**: the REQUIRED facts are now `slot0.pid != 0 &&
   slot0.dispatches >= 1` (spawned + provably scheduled — readable during or
   after life) while `slot7` stays `pid == 0 / dispatches == 0`
   (discrimination). The alive observation (`state >= 1`) is printed
   opportunistically but no longer asserted — same reasoning as the DDR-730
   `smoke-agent-click` change: transient live states are not deterministic
   serial witnesses on slow hosts. The poll window is 120 **RTC seconds**
   (`SYS_CLOCK`, wrap-handled) instead of an iteration count, which mis-sizes
   across host speeds.

**Harness fix (unrelated flake surfaced here):** `boot_test.sh` wrote its serial
capture to a `mktemp` in `/tmp`; on the dev WSL host `/tmp` is wiped mid-run,
truncating long-timeout gates and failing them spuriously. `SERIAL_LOG` is now
overridable (default unchanged, so CI is unaffected) — local runs point it at a
persistent path. Not a code issue; the feature verifies deterministically.

## Non-goals

- No TSC-granularity accounting (the 100 Hz tick is the system's time base;
  cycle-accurate accounting is a different, per-switch-cost slice).
- No compositor rendering change (hardening 3/3 decides how to draw it).
- No scheduler-policy use of the counters (observability only).
