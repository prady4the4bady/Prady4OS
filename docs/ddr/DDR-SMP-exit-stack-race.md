# DDR — exit-vs-collect kstack use-after-free (cap-4 fallout, CI-caught)

## The bug
`sched_exit` set ZOMBIE (+ `on_cpu=-1`), **released** `g_sched_lock`, woke the
waiter, then called `schedule()` to switch away. Once cap-4 let user threads
(and their wait4-ing parents) run on any CPU, a parent on another CPU could
wake, collect the zombie, and `sched_destroy` it — freeing the kernel stack —
in the window before the dying thread's `context_switch` finished executing ON
that stack. KASAN's freed-page poison made the crash legible: a kernel #GP with
`RAX=0xDEADBEEFDEADBEEF` right after a (legitimate) W^X user-kill, CI-only
(TCG interleaving; KVM never lost the race locally). Single-CPU could never
hit this — the collector only ran after the dying thread had switched away.

## The fix
The locks-1 switch-lock handoff, applied to exit: `sched_exit` now holds
`g_sched_lock` **across** its final switch. `schedule()` splits into
`schedule_locked(fl)` (the body; caller already holds the lock) and the
`schedule()` wrapper. `sched_exit` acquires once — reparent walk, ZOMBIE,
`on_cpu=-1`, waiter wake (an atomic CAS, safe under the spinlock) — then calls
`schedule_locked(fl)` WITHOUT releasing. The lock is released by the handoff
(the next thread's `irq_restore` / `thread_trampoline`), which happens strictly
after `context_switch` has left the dying stack. Any collector's
`sched_destroy` takes `g_sched_lock` first, so it serializes behind that
release — the stack is provably no longer in use when freed.

`thread_trampoline`'s DONE path is unaffected (DONE kernel threads are never
`sched_destroy`'d), but it gets the same treatment for uniformity of the
exit-switch discipline where applicable.

## Gate
No new gate: `smoke-syswait`/`smoke-user` (the W^X kill + wait4 paths) exercise
it every run; the race itself is gone by construction (lock ordering), not by
timing.
