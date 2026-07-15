# DDR-754 — `ps` CPU accounting: per-process CPU time

**Status:** proposed (pre-code)
**Layer:** proc + user (PRISM). Read-only observability; extends DDR-743 `ps`.

## Problem

`ps` (DDR-743) lists pid/ppid/state/user/name but no CPU usage — you can see
*that* a process exists, not how much CPU it has taken. The counters already
exist: DDR-735 added per-tcb `run_ticks` (100 Hz ticks observed while current)
and `dispatches` (switch-in count), incremented for *every* thread, not just
agents. Nothing surfaces them to `ps`.

## Decision

Extend the `SYS_GETPROCS` snapshot with the two counters (read-only, no
scheduling-logic change):

- `struct procinfo` (sched.h) gains `uint64_t run_ticks, dispatches;`.
- `sched_snapshot` copies them from the tcb (already under `g_sched_lock`).
- PRISM `ps` prints a CPU-time column — `run_ticks * 10` ms (the tick is
  100 Hz) — and a switch count. New header:
  `  PID  PPID S U    CPUms   DISP NAME`.

The `procinfo` struct grows; its only consumer is PRISM's `ps` (SYS_GETPROCS has
no freestanding probe), so the kernel struct and the PRISM mirror are updated in
lockstep — `copyout` uses the kernel `sizeof`, which now matches the mirror.

## Gate — extend `smoke-shell` (no new gate; stays 90)

The DDR-743 assert keyed on the old header `PID  PPID S U NAME$`; update it to the
new header shape `PID +PPID S U +CPUms +DISP NAME$`. This still proves
`SYS_GETPROCS` round-tripped, now including the accounting columns. (Exact CPU
figures vary with scheduling, so only the header shape is asserted — deterministic.)

## Non-goals

- No %CPU / rate over an interval, no top-style live refresh — cumulative ticks
  only.
- No per-CPU breakdown, no kernel-vs-user time split.
- `run_ticks` is sampled at the 100 Hz tick (10 ms granularity), so sub-10 ms
  threads may read 0 — expected for a coarse sampled counter.
