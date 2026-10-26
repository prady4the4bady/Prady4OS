# DDR-1186: opt-in CPU placement for a thread (C1.4 P1d)

Status: **proposed**. It is stacked on P1c (#76). Platform: QEMU TCG with 4 vCPUs. **Not verified on real hardware.**

## 1. Why

DDR-1181 §4.3 measured that smoke-thread cannot put two threads of one process on two CPUs. Run [40276219205](https://github.com/prady4the4bady/Prady4OS/actions/runs/40276219205) placed 0 of 192 attempts, and the mechanism is in the source:

- a new or woken thread is queued on the CPU that woke it (`rq_push(self, t)` in `sched_unblock`; `rq_push(this_cpu)` in `sched_create_state`);
- a CPU takes another CPU's work only when its own queue is empty (`rq_pop` first, `rq_steal` only on empty);
- during the boot's background load every CPU's queue holds work (`rqq=15`), so nothing is ever stolen.

Because of this, three things stay **not verified** end to end:
- cross-CPU TLB coherence for threads (DDR-1177 C1.4 items D, E and M4);
- the uaccess fixup under a racing sibling (DDR-1181 §3.7, arm G);
- the mutants T4, T5, T6 and T8.

The in-kernel smoke-tlbshoot (DDR-1177 arms A–C) gates the shootdown mechanism. It does not show user threads seeing it.

## 2. Options

1. **Wake-time balancing.** Queue a woken thread on the least-loaded CPU. This changes placement for **every** thread. DDR-885 keeps placement unchanged while the lost-thread work (DDR-880/884) is open, so a change here would make the next red there impossible to attribute. **Rejected.**
2. **Use the timed-wake path.** A timed-out sleeper is woken by whichever CPU's `sched_tick` walk sees the deadline first, and `timer_tick` runs that walk on the BSP right after it advances `g_ticks` (`idt.c`). So every sleeper lands on the BSP, and a probe built on this would depend on an accident of tick order. **Rejected.**
3. **An opt-in placement call (chosen).** A thread asks to run on one CPU only. Threads that never ask are placed exactly as today.

## 3. Design

**NSI 118, `SYS_SCHED_CPU (op, cpu)`.** 117 is `SYS_FUTEX` on P2 (#81); 118 is checked free on every open branch.

| op | Effect | Returns |
|---|---|---|
| 0 `GET` | — | the CPU index the caller runs on now |
| 1 `PIN` | the calling thread runs on `cpu` only, from its next switch on | 0, or `-EINVAL` if `cpu` is not a present CPU |
| 2 `UNPIN` | placement as before | 0 |

- **One field.** `struct tcb` gets `int pin_cpu`, which is -1 when the thread is not pinned. `kmalloc` does not zero, so `sched_create_state` initialises it explicitly (CLAUDE.md §11). `init_idle` sets it to -1 as well, since its `memset` leaves 0, which would read as "pinned to CPU 0". Idles are never queued, so this is defensive only.
- **Not inherited.** Fork children and new threads start unpinned. This diverges from Linux, where affinity is inherited. Inheriting it would let one pinned parent silently move every process it spawns.
- **Enqueue.** `rq_push(cpu, t)` uses `t->pin_cpu` instead of `cpu` when the field is ≥ 0. Every path that makes a thread runnable goes through `rq_push`: create, unblock, the re-queue of a preempted `prev`, and the contended push-back. So this one site covers them all. For an unpinned thread the path is the same as today, plus one compare.
- **Steal.** `steal_pass` already holds the victim queue's lock (a trylock). It skips a victim whose **head** is pinned to another CPU. If `rq_take` still returns a thread pinned elsewhere (because stale non-READY entries ahead of it were dropped), the thief pushes it back, onto its pin CPU, and tries the next victim. `rq_take` has cleared `rq_on`, so the push cannot be dropped. This push-back is the same pattern `schedule_locked` uses when `switch_wait_offcpu_sched` gives up.
- **Idle.** `rq_has_ready` decides whether an idle CPU may `hlt`. It now ignores a queue whose head is pinned to another CPU. Without this, an idle CPU would steal the pinned thread, push it back, see "ready" again, and spin without halting, all while the thread waits for its own CPU. Under TCG that spin costs a host core. Because the head is dereferenced, it is read under the queue's lock (a trylock with IRQs off); a busy lock counts as ready, as before.
- **The cost of the head rule.** A queue whose head is pinned hides the unpinned entries behind it from thieves until its own CPU pops the head. This only happens when pins are in use. With no pinned thread, every queue head has `pin_cpu = -1` and both paths behave exactly as before.
- **Taking effect.** `PIN` sets the field and yields. `schedule_locked` re-queues `prev` through `rq_push`, which lands it on the pin CPU, and that CPU picks it at its next schedule (at most one 10 ms tick).
- **Who may call it.** Any thread, for itself only. There is no `tid` argument. A thread on one CPU can starve nothing that a spinning thread cannot starve today.

## 4. Gate changes (smoke-thread)

| Arm | What | Sentinel |
|---|---|---|
| P (new) | pin to each present CPU, yield, and `GET` returns it; `PIN` with an absent CPU returns `-EINVAL`; `UNPIN` returns 0 | `PRADYOS_THREAD_P pinned_ok=1 einval=1` |
| D, E | DDR-1181 §4.1's concurrency-proven trials, back as cross-CPU arms. The creator pins to CPU 1 and the loop thread to CPU 2. A trial counts only when the creator's spin (with no syscall) saw the loop thread's counter advance with the vDSO clock still; at least 12 of 16 such trials are required | `prot_stale=0 faulted=1 placed_ok=1`, and the same for E |
| M | poison over the placed E trials | `PRADYOS_THREAD_M poison=0` |
| G | the copier pinned to CPU 1 and the toggler to CPU 2; at least one `-EFAULT` must leave A's tail written | `PRADYOS_THREAD_G survived=1 midcopy_ok=1` |

**Mutants.** Each lands on a different arm.

| Mutant | Change | Must be red on |
|---|---|---|
| P1 | `rq_push` ignores `pin_cpu` | P (`GET` differs from the pin) |
| T4 | `vmm_protect_range` skips `tlb_shoot` | D (`prot_stale>0`) |
| T5 | `vmm_unmap_range_flush` skips `tlb_shoot` | E (`stale>0`) |
| T6 | free before the shootdown (DDR-1177 M4) | M (`poison>0`) |
| T8 | the #PF handler ignores the uaccess fixup range | G (kernel panic) |

**Vacuity check before writing the arms.** On the unfixed tree (P1c, no pin call) the probe cannot build, because the call does not exist. With P1 (pin ignored), D and E fall back to the P1c behaviour, so `placed_ok=0`, and they are red. That is why P must be red **on its own line**, so P1 is distinguishable from a placement flake.

## 5. Residual risk

- **Time-sharing on the pinned CPU.** CPUs 1 and 2 still run background work, so the two pinned threads overlap only when both are scheduled at once. The 12-of-16 bound is set before measurement. If correct code is red because of it, the bound is not lowered. The placement counts are read and the cause is named first.
- **The BSP is never used for a pin.** The BSP runs the timer bookkeeping and IRQ1, and is excluded so that its tick work does not count against the probe.

## 6. Honesty labels

- `SYS_SCHED_CPU` will be gated by smoke-thread arm P once the arm is built. Until then it is **not verified**.
- QEMU TCG only; cross-CPU timing under TCG is not hardware timing.
- Claims nothing about U1–U3 or OPEN-1/2/12/13.
