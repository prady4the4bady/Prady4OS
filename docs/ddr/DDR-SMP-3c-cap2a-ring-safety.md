# DDR-SMP-3c-cap-2a — SMP-safe scheduler internals (ADR-031 D2/D3, part 1)

> DDR before code. Splits the ADR-031 cap-2 work: **2a** makes the scheduler's
> ring/topology safe for concurrent CPUs *without* yet letting APs schedule, so
> the change is a behavioral **no-op on the BSP** and all 58 gates prove it by
> non-regression. **2b** then flips APs into `schedule()` (per-CPU idle + wake
> IPI + a gate). Landing the hot-path internals rewrite separately de-risks it.

## The hazard cap-2b would otherwise expose
The scheduler is one shared circular ready ring walked from each CPU's
(per-CPU, `%gs`) `current`. `schedule()`'s `runnable()` treats `THREAD_RUNNING`
as pickable — fine on one CPU, but once a second CPU walks the same ring it
could pick a thread **currently RUNNING on another CPU** → the same thread
executes on two CPUs. And `sched_create` inserts into the ring, `sched_exit`
reparents + zombifies, all *without* the scheduler lock — concurrent ring
mutation would corrupt the links.

## Decisions
- **D1 — `on_cpu` claim.** `struct tcb` gains `int on_cpu` (−1 = not running
  anywhere). `schedule()` picks only `state==READY && on_cpu<0`; on switch it
  claims `next->on_cpu = this_cpu` / `next->state=RUNNING` and releases
  `prev->on_cpu = -1` (if `prev` was RUNNING). All under `g_sched_lock`, held
  across `context_switch` (the locks-1 switch-lock handoff), so a claim is
  atomic — two CPUs can't grab one thread. On one CPU today: `prev` is the only
  RUNNING thread, every other READY thread has `on_cpu<0`, so the pick set is
  identical to `runnable()` minus `prev` — behavior unchanged.
- **D2 — topology under `g_sched_lock`.** `sched_create` (ring insert),
  `sched_exit` (reparent walk + ZOMBIE + waiter capture), and `sched_destroy`
  (ring unlink) take `g_sched_lock`; DDR-SMP-3c-locks-1 §D2's BSP-only topology
  restriction is superseded (ADR-031). The reaper already scanned under the
  lock; it now unlinks the victim under the lock and frees its resources
  (`kfree`/`vmm_destroy`) *outside* it (freeing must not hold a leaf lock). No
  self-recursion: no `sched_create/destroy` caller holds `g_sched_lock`
  (`sched_create_user_clone` uses a bare `cli`, not the lock).
- **D3 — BLOCKED-create for user paths (the create-then-init race).** A user
  thread is created, then its `cr3`/`user_rip`/authority are set — a window
  where a *second* scheduling CPU could run it half-initialized (the same class
  as DDR-boot-authority-race). So the user-creation paths insert the thread
  **BLOCKED** atomically (under the lock, before release) and the caller
  `sched_unblock`s it once fully initialized. Implemented by an internal
  `sched_create_state(entry,arg,name,initial_state)`: `sched_create` stays
  `READY` (kernel threads are fully runnable at insert — no post-init, so their
  13 call sites are unchanged); `sched_create_user` and `_clone` pass BLOCKED.
  `sched_create_user` already returned BLOCKED (its post-`state=BLOCKED` set,
  now done atomically at insert); `_clone` gains a closing `sched_unblock` and
  drops its now-redundant `cli` guard (BLOCKED-at-insert subsumes it).

## Why this is a BSP no-op
Only the BSP schedules in 2a (APs stay parked). With one scheduler, `on_cpu` is
always the BSP or −1, the pick set is unchanged, the topology locks are
uncontended, and BLOCKED-create + immediate/So caller unblock reproduces today's
timing. The 58 gates must stay green with zero behavior change — that IS the
proof the internals are correct before 2b adds real concurrency.

## Gate
No new gate. All 58 re-verify the no-op. 2b adds `smoke-smpsched`.

## Non-goals (→ cap-2b / cap-3)
Per-CPU idle threads; APs entering `schedule()`; reschedule/wake IPIs on
create; per-AP LAPIC-timer preemption; per-CPU runqueues.
