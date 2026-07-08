# DDR-SMP-rq-2 — remove `g_sched_lock` from the context-switch path

> DDR before code. rq-1 made the PICK O(1) per-CPU, but `schedule_locked` still
> holds `g_sched_lock` **across `context_switch`**. That single global lock on
> the hottest path serializes every CPU's switch. Removing it means replacing
> the invariant it was protecting with an explicit handshake.

## What the lock was actually protecting
Three distinct hazards, all rooted in one fact: **a thread's `rsp` is not saved
until `context_switch` executes, and its kernel stack is live until then.**

1. **Stale-`rsp` resume.** `schedule_locked` re-enqueues a preempted `prev`
   (`rq_push`) BEFORE `context_switch` saves `prev->rsp`. A thief on another
   CPU could pop `prev` and switch TO it, loading a stale/garbage `rsp`.
   Today the thief blocks on `g_sched_lock` (released only by the handoff,
   after the switch).
2. **Exit-vs-collect UAF** (DDR-SMP-exit-stack-race): a `wait4`/reaper collector
   frees the dying thread's kernel stack while it still executes on it. Today
   `sched_exit` holds the lock across its final switch and `sched_destroy`
   blocks on it.
3. **Double-run**: two CPUs claiming one thread — already handled by the
   `on_cpu` claim under the rq lock (cap-2a D1), NOT by `g_sched_lock`.

So (3) survives untouched; (1) and (2) need a replacement.

## Decisions
- **D1 — `on_cpu` becomes the off-CPU handshake, not just a claim.**
  `on_cpu >= 0` now means "**still executing / stack not yet saved**", and is
  cleared with a RELEASE store only AFTER `context_switch` has left the stack.
  It is no longer cleared by the outgoing CPU before the switch.
- **D2 — `finish_task_switch` via `percpu.prev`.** The outgoing CPU stores the
  outgoing tcb in `this_cpu()->prev` immediately before `context_switch`. The
  **resumed** thread (whichever CPU it wakes on) reads `this_cpu()->prev` — the
  thread THAT CPU just switched away from — and does
  `__atomic_store_n(&prev->on_cpu, -1, RELEASE)`. `thread_trampoline` (a brand
  new thread's first entry, which has no resumed `schedule_locked` frame) does
  the same as its first act, alongside its existing lock release.
  Reading `percpu.prev` AFTER the switch is correct precisely because `%gs` is
  per-CPU: it names the CPU we are now on, and that CPU set it.
- **D3 — the pick side spin-waits.** Before `context_switch` loads `next->rsp`,
  the picking CPU waits `while (__atomic_load_n(&next->on_cpu, ACQUIRE) >= 0)
  pause;` — the acquire pairs with D2's release, so `next->rsp` is visible and
  final. Closes hazard 1. (Skip the wait when `next == prev`; a CPU never waits
  on itself.) This is a bounded wait: the holder is mid-switch, not blocked.
- **D4 — freeing waits on the same flag.** `sched_free_tcb` (and thus
  `sched_destroy` and the reaper) first spin-waits `on_cpu < 0` on the victim.
  Closes hazard 2 WITHOUT `g_sched_lock`, so `sched_exit` no longer needs to
  hold the lock across its final switch (`schedule_locked(fl)` reverts to
  `irq_restore(fl); schedule();`). The DDR-SMP-exit-stack-race guarantee is
  preserved by construction — the collector cannot free a stack whose owner has
  not yet released it.
- **D5 — what `g_sched_lock` still covers.** Ring topology only:
  `sched_create`'s insert, `sched_exit`'s reparent walk + ZOMBIE, `pid_alive`,
  the reaper scan, `sched_ring_unlink`. It is NEVER held across
  `context_switch` again, and the `schedule()` hot path does not take it at all
  — only the per-CPU rq leaf lock (and a victim's rq lock while stealing).
  `irq_save/irq_restore` keep local IRQs masked across the switch (RFLAGS is
  preserved by `context_switch`, as before).

## Why the switch is safe with interrupts masked and no global lock
Between `rq_push(prev)` and `context_switch`, a thief may pop `prev` — it then
spins in D3 until we release in D2. The window is a few instructions and cannot
deadlock: the spinner holds no lock the switching CPU needs (it released the
victim's rq lock before spinning — the D5 leaf discipline), and the switching
CPU never waits on the spinner.

## Gate
No new gate: `smoke-rqstress` (24-thread storm), `smoke-smpuser`, `smoke-blkmq`,
`smoke-syswait` and the whole 73 exercise pick/steal/exit/reap under 4 CPUs
every run. The proof is non-regression plus stress determinism (rqstress ×10).
73 gates.

## Non-goals
Per-wake `smp_resched_one` IPIs (a separate slice); affinity; priority classes;
lock-free rqs (the leaf locks stay).
