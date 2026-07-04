# DDR-SMP-3c-locks-4 — IPC endpoint + broadcast bus locking (ADR-030 stage 3c)

> DDR before code. Continues the full-3c prerequisite campaign: the synchronous
> IPC endpoint and the sovereign broadcast bus.

## The cross-CPU lost-wakeup
`ipc_send/ipc_recv` and `bcast_publish/bcast_wait` close their lost-wakeup race
with `cli/sti` — but that masks only the **local** CPU's interrupts. Across
CPUs it is no serialization at all:

1. Receiver reads the queue empty, is about to record itself as the waiter.
2. Sender on another CPU delivers, reads `waiter == 0` (not recorded yet),
   delivers no wakeup.
3. Receiver records itself + blocks → sleeps forever.

`sched_unblock` is already an atomic CAS (locks-1), but a CAS on a thread that
is **not yet BLOCKED** is a no-op — so the wakeup is still lost unless the
waiter's BLOCKED state is published *before* the waker can observe the queue.

## Decisions
- **D1 — per-object spinlock.** `struct ipc_endpoint`, `struct bcast_bus`, and
  each `struct bcast_subscriber` gain a `spinlock_t`. Senders/publishers and the
  subscribe-list mutation take it with `spin_lock_irqsave` (keeps the old local
  no-preempt property AND adds cross-CPU exclusion).
- **D2 — publish BLOCKED under the lock (the crux).** New scheduler primitive
  `sched_block_on(spinlock_t *lk)`: with `lk` held, it sets the caller
  `THREAD_BLOCKED`, releases `lk`, then `schedule()`s; on return it re-takes
  `lk` for the condition re-check. Because BLOCKED is set **under** `lk`, a
  waker serialized after the release always sees BLOCKED, so its CAS can't miss.
  If the waker fires in the gap before `schedule()`, it CASes us READY and
  `schedule()` simply re-queues us (a benign spurious wake — the `while`
  re-checks). IRQs stay masked across the switch, exactly as the prior `cli`
  path did (`context_switch` preserves RFLAGS).
- **D3 — the async SPSC ring is already correct; no change.** `ipc_ring`'s
  head/tail use acquire/release atomics with a strict single-producer /
  single-consumer contract, so one sender and one receiver on different CPUs are
  already race-free by construction. Adding a lock would only slow it. Its
  invariant (exactly one producer, one consumer per ring) is the thing to guard
  in review, not the code. The bcast per-subscriber queue is SPSC too, but its
  `waiter` handoff needs the lock (D2), so the bus takes it.

## sched_block_on vs sched_block
`sched_block()` (set BLOCKED; schedule()) stays for callers with no companion
lock (the reaper, block/net waiters that re-check a flag the IRQ sets). The new
primitive is only for the "verified a condition under a lock, now sleep on it"
shape — it splices the lock release between BLOCKED and the switch.

## Gate
None new. `smoke-agents`/`smoke-aether`/`smoke-aether-queue`/`smoke-aether-sec`
drive the endpoint + bus (AETHER approve/mode traffic) and the SPSC ring every
run; single-CPU behavior is unchanged. 58 gates.

## Non-goals
Multi-producer/consumer rings (the SPSC contract stands); fairness/priority on
the waiter handoff; per-subscriber-queue growth.
