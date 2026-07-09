# DDR-SMP-rq-3 — per-wake reschedule IPIs

> DDR before code. rq-2's deferred half. `sched_unblock` enqueues a freshly
> READY thread on the waker's own CPU; an *idle* CPU only picks it up (by work
> stealing) on its next `schedule()`, which for an idle AP is its 100 Hz timer
> tick — up to ~10 ms of wake latency. A directed IPI makes it immediate.

## Decisions
- **D1 — `percpu.idle` flag.** The AP idle loop (`sched_ap_enter`) sets
  `this_cpu()->idle = 1` right before `sti; hlt` and clears it on wake. A plain
  cross-CPU-visible byte; the BSP is never marked idle (it always has the
  FS-phase / demo work and its own timer).
- **D2 — `smp_resched_one(cpu)`.** Sends the existing wake IPI (vector 49,
  whose ISR just EOIs and breaks `hlt`) to one CPU. A counter `g_resched_ipis`
  records directed kicks (the gate's observable).
- **D3 — `sched_unblock` kicks an idle CPU.** After `rq_push`, scan for ANY
  idling non-self CPU and `smp_resched_one` it. The kicked CPU wakes, its idle
  loop runs `schedule()` → `rq_steal` grabs the thread within microseconds.
  Enqueue target is unchanged (waker's own CPU) — the kick just triggers a
  prompt steal; locality is preserved.
- **D4 — the wake/enqueue race is closed by a double-check, backstopped by the
  timer.** Race: a CPU decides to idle (queues empty) but before it sets
  `idle=1`, a waker enqueues + reads `idle==0` and does NOT kick → the thread
  waits for the timer tick. The idle loop closes this: after `schedule()`
  returns idle it sets `idle=1`, then RE-CHECKS `rq_any_ready()` (a lockless
  head hint) and loops without hlting if work appeared. A missed hint is still
  caught by the 100 Hz tick — correctness never depends on the IPI, only
  latency does.

## Gate
`smoke-resched` (`-smp 4`): the FS phase does abundant cross-CPU block/unblock
(crosswake, blk completions) with idle APs present, so directed kicks fire;
the BSP asserts `[smp] resched OK` when `g_resched_ipis > 0`. Correctness is
the existing 73 (rqstress/smpsched/blkmq unchanged). 74 gates.

## Non-goals
Pushing directly onto the idle CPU's queue (steal-after-kick is simpler and
keeps one enqueue path); IPI coalescing; wake-affinity heuristics.
