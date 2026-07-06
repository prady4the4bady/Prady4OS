# DDR-SMP-rq-1 — per-CPU runqueues + work stealing

> DDR before code. The last structural SMP item: every `schedule()` on every
> CPU currently walks ONE shared ring under ONE `g_sched_lock` — correct
> (ADR-031) but contended, O(threads) per pick, and every pick fights every
> other CPU. Split the "ready queue" role from the "all-threads list" role.

## Decisions
- **D1 — the ring stays, as the THREAD LIST only.** `tcb.next` (the circular
  ring) remains the topology: reparent walks, `pid_alive`, the reaper scan,
  wait4 lookups. Still mutated under `g_sched_lock` (create/exit/destroy are
  cold paths). It is no longer consulted by the scheduler hot path.
- **D2 — per-CPU ready queues.** `struct percpu` gains an intrusive FIFO
  (`tcb.rq_next`; head/tail in a new `struct rq { spinlock_t lock; tcb *head,
  *tail; }` indexed by cpu). `schedule()` pops from ITS OWN rq (own lock only);
  a preempted-but-runnable prev is pushed back to its own rq. Idles are never
  enqueued (per-CPU fallback, as today).
- **D3 — wake/placement.** `sched_unblock` (CAS BLOCKED→READY, unchanged) now
  also ENQUEUES the thread: to the waker's own rq if `on_cpu<0` (it must be —
  READY threads off-queue only exist transiently), then `smp_resched_one(cpu)`
  (wake IPI) if the target rq's owner idles. BLOCKED-create's final unblock
  places new threads the same way. User threads keep no affinity (any rq).
- **D4 — work stealing.** A CPU whose rq is empty (before falling to idle)
  scans other rqs (their locks, trylock to avoid convoying) and steals ONE
  thread (respecting `pickable`'s rules: never an idle; user threads are fine
  everywhere since cap-4). Steal keeps all CPUs busy without a balancer.
- **D5 — lock order.** rq locks are leaves: never held while taking
  `g_sched_lock` or another rq's lock EXCEPT in steal, which orders "own rq
  released first, then trylock victim" (no hold-and-wait → no deadlock).
  `g_sched_lock` shrinks to: topology mutation, the exit switch handoff
  (DDR-SMP-exit-stack-race — unchanged), and `context_switch` protection.
  The switch itself still runs under `g_sched_lock` this slice (rq-2 may
  move to per-CPU switch locks); the win here is the PICK no longer scans.

## Implementation findings (bugs the gates caught)
- **Idle starvation:** idles are never enqueued, so "keep prev if runnable on
  empty queues" starved the own idle — which IS a real context (the BSP idle
  runs `sched_demo`; the bench yield ping-pong hung). A non-idle prev now
  always rotates through `g_idle[cpu]` on empty queues (prev re-queues at the
  tail) — the cap-2b lesson, rq edition.
- **Transient-drop lost threads:** an unblock can race its target's
  switch-away (CAS READY while `on_cpu>=0` — the cap-2a spurious-wake case).
  The FIFO's "drop stale entries" then LOST the thread (deterministic
  `smoke-winops` hang). `rq_take` now RE-APPENDS READY-but-claimed transients
  and drops only non-READY entries.
- **Scheduling-speed assumptions in tests:** per-CPU queues let the surface
  client sprint through create→close before a later-spawned compositor even
  initialized — `SURFACE_GONE` needs the compositor to have composited the
  3-set. The FS phase now spawns COMPOSIT before SURFTEST (the natural desktop
  order) and the client's close delay widened.
- **D3 deviation:** no per-wake IPI this slice — an idle AP picks fresh work
  up via its own 100 Hz tick (≤10 ms) or an explicit `smp_resched_all` at the
  spawn sites; per-wake `smp_resched_one` is an rq-2 optimization.

## Gate
`smoke-rqstress` (`-smp 4`): a fork-storm/thread-storm stress — N kernel
threads created in waves across CPUs, all must complete (counted), plus the
existing 63. Total 64.

## Non-goals (→ rq-2+)
Per-CPU switch locks (removing `g_sched_lock` from the switch path); affinity
API; load metrics; NUMA; priority classes.
