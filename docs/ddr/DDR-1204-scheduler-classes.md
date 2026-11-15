# DDR-1204: Scheduler classes (C1.5): real-time FIFO/RR, kmutex priority inheritance, deadline, PSI, high-resolution time

Status: **design, before any code.** C1.5 in `docs/V1_MASTER_CHECKLIST.md`; ordered by #45 6761846242 item 4 ("then C1.5, C1.6, C1.7 ... each with a DDR first"). Priority inheritance was moved here from C1.4 by DDR-1193 D1.

## 1. What exists (by reading, at `debb6a4`)

- One ready queue per CPU (`struct rq` in `kernel/proc/sched.c`: a lock and a FIFO through `tcb->rq_next`). There is no priority field and no scheduling class.
- `rq_pop` picks the READY thread with the lowest `dbg_vruntime` (`fair_candidate`), charged by `sched_charge_elapsed` with `tcb->weight` (1024 default). Every thread is in this one fair class.
- The timer tick is the PIT at 100 Hz (`g_ticks`); there is no high-resolution timer and no tickless idle.
- A directed reschedule IPI exists (`smp_resched_one`, `g_resched_ipis`), and pinning exists (`tcb->pin_cpu`, DDR-1186; `SYS_SCHED_CPU` 118).
- `struct kmutex` (DDR-1181 §3.3) is a sleeping mutex with a FIFO of waiters and direct hand-off. It has no priority inheritance.
- The next free syscall number is 120 (`SYS_MSYNC` is 119).

## 2. Decisions

**D1. Classes are an ordered pick, not a new queue structure.** Each CPU gets an RT queue beside the fair FIFO: 100 FIFO lists, one per RT priority 1..99 (index 0 unused), and a 128-bit bitmap of non-empty lists. `rq_pop` takes the head of the highest non-empty RT list; only if every RT list is empty does it run today's fair pick. Deadline threads (D5) are picked before RT. The fair class is unchanged, so no existing gate's picking changes while no thread is RT.

**D2. Two new tcb fields, both set in `sched_create_state` (CLAUDE.md rule 11).** `sched_class` (FAIR=0, FIFO, RR, DEADLINE) and `rt_prio` (0 for fair). `kmalloc` does not zero, so both get explicit initialisers. Inherited priority (D4) is a third field, `rt_prio_eff`, also initialised there.

**D3. Preemption.**
- A wakeup (`sched_unblock`, `rq_push`) of a thread whose effective class and priority beat the target CPU's current thread sets that CPU's need-resched flag and, when the target is another CPU, sends `smp_resched_one`.
- RR: a slice of 10 ticks (100 ms at 100 Hz). `sched_tick` decrements it; at zero the thread goes to the tail of its priority list and the CPU reschedules. FIFO has no slice.
- A fair thread never preempts an RT thread.

**D4. kmutex priority inheritance (DDR-1193 D1).**
- A tcb records the kmutex it is blocked on (`kmx_blocked_on`).
- When a waiter W blocks on M owned by O, `kmutex_lock` raises O's `rt_prio_eff` to W's effective priority if that is higher. It requeues O if O is on a ready queue, then follows O's own `kmx_blocked_on` to the next owner. The walk stops after 8 links, and a cycle stops it at once; a cycle is a deadlock and is counted for the deadlock-detection item.
- On `kmutex_unlock`, O's `rt_prio_eff` drops to the maximum of its base `rt_prio` and the top waiter of every kmutex it still holds. A small per-tcb list of held kmutexes makes that computable.
- The waiter FIFO stays FIFO among equal priorities. The hand-off goes to the highest-priority waiter, then FIFO order (D1's rule applied to waiters).
- Futex-based user locks (`psync.h`) get no PI here; `FUTEX_LOCK_PI` is out of scope and stated so.

**D5. Deadline class (part P2), EDF with admission control.**
- Parameters per thread: runtime, deadline, period, in ticks.
- Admission: the sum of runtime/period over admitted threads on a CPU must stay at or below 95% (D6), else `-EBUSY`.
- The pick is earliest absolute deadline. A thread that uses its runtime before its deadline is throttled until its next period.
- On 100 Hz ticks the granularity is 10 ms; P4 (high-resolution timers) makes it finer. Stated limit: on TCG, timing is emulated, so deadline-miss counts measure the scheduler's logic, not real-time behaviour.

**D6. RT throttling.** RT and deadline threads may use at most 95 ticks of every 100 on a CPU. The rest goes to the fair class, so an RT spinner cannot starve the reaper, the RCU tick work or a shell. A throttled CPU counts the event (`rt_throttled`).

**D7. Syscalls (part P1b).** These are scheduling-only, so no capability bit beyond the existing ones is needed for fair-to-fair. Raising to an RT or deadline class needs the same check as the existing privileged scheduling calls: `-EPERM` for an unprivileged caller.
- 120 `SYS_SCHED_SETSCHED (tid, class, prio)`, returning 0 or `-EINVAL|-EPERM|-ESRCH|-EBUSY`.
- 121 `SYS_SCHED_GETSCHED (tid)`, returning `(class << 8) | prio`.
- 122 `SYS_SCHED_SETDL (tid, runtime, deadline, period)` (P2).

**D8. Pressure stall information (part P3).**
- Per CPU, with system totals: "some" stall time for CPU (READY but not running while another thread runs), memory (blocked in reclaim or on a page fault that waits for a frame) and I/O (blocked in a block-device wait).
- Tracked as stall-start and stall-end timestamps on the existing state transitions, summed in ticks, and reported as 10 s, 60 s and 300 s averages through the existing stats path.
- Memory and I/O "some" depend on the wait sites being tagged; P3 lists each site it tags.

**D9. High-resolution clocks and tickless idle (part P4).**
- The LAPIC timer in one-shot TSC-deadline mode (where CPUID reports it), or LAPIC one-shot, programmed to the next event: the RR slice end, the next timed-wait deadline (the DDR-1200 sweep stays as the fallback) or a deadline-class period.
- Idle CPUs stop the periodic tick.
- `clock_gettime(CLOCK_MONOTONIC)` from the TSC, calibrated against the PIT at boot.
- CPU frequency control (P-states, `MSR_IA32_PERF_CTL`) is **not verifiable under QEMU TCG**. P4 implements the driver only behind CPUID detection and labels it "not verified".

## 3. Parts and gates

Each part is its own PR with its own gate and mutants, in this order:

| part | content | gate |
|---|---|---|
| P1a | D1–D4, D6 (kernel only) | `smoke-sched` (flag build, `SCHED_TORTURE=1`, like `smoke-rcu`) |
| P1b | D7 syscalls 120–121, a user arm | `smoke-sched` user arm via `/bin` test |
| P2 | D5 deadline, syscall 122 | `smoke-sched` arm SD |
| P3 | D8 PSI | `smoke-psi` |
| P4 | D9 timers, tickless, TSC clock | `smoke-hrt` |

### 3.1 `smoke-sched` arms for P1a (kernel threads, all pinned to the last CPU unless stated)

| arm | setup | asserts |
|---|---|---|
| SF | an RT FIFO prio-50 spinner and a fair spinner on the same CPU, 200 ticks | the fair spinner's ticks in that window are at most the D6 allowance (≤ 5% + 1 tick slack); the FIFO thread's are at least 190 |
| SR | two RR prio-40 spinners on one CPU, 200 ticks | each runs ≥ 80 ticks and the run alternates at least 15 times (slice expiry seen) |
| SP | five RT threads of priorities 10..50 made READY together, in reverse order | they first run in priority order 50, 40, 30, 20, 10 |
| SW | a prio-60 FIFO thread woken from another CPU while a fair spinner runs on its CPU | it runs within 2 ticks (`smp_resched_one` sent; preemption on wake) |
| SI | priority inversion: low-prio (10) O takes kmutex M; high (60) H blocks on M; medium (30) spinner S is woken, all on one CPU | H gets M within 3 ticks of O becoming runnable; with PI O runs at 60 and S does not run before O releases M |
| SC | a 3-link chain O1←O2←H across two kmutexes | O1 runs boosted to H's priority (the walk follows the chain) |
| ST | an RT spinner alone on a CPU for 200 ticks, with a fair thread queued | the fair thread gets ≥ 8 ticks (throttling, D6) and `rt_throttled > 0` |

Every arm is checked for vacuity on today's tree: SF, SR, SP, SW, SI, SC and ST all fail without P1a, because no class exists. That is the point of a design-first gate. The check that counts is the mutant column.

### 3.2 Mutants (one change each, on the final head)

| mutant | change | red on |
|---|---|---|
| S1 | `rq_pop` ignores the RT lists (fair pick only) | SF, SP |
| S2 | the RR slice is never reset or expired | SR |
| S3 | a wakeup does not preempt (no resched flag, no IPI) | SW |
| S4 | `kmutex_lock` does not boost the owner | SI |
| S5 | the PI walk stops after one link | SC |
| S6 | no RT throttling | ST |
| S7 | `kmutex_unlock` does not drop the boost | none of the arms above. Stated **possibly not gated** unless SI's post-release check (O back at prio 10 runs after S) catches it; P1a measures this and records the result. |

## 4. What this does not do

- No `FUTEX_LOCK_PI`; user locks get no inheritance.
- No RT group scheduling or cgroups.
- No real-time guarantees: QEMU TCG emulates time, so every timing figure is the scheduler's logic under emulation. Nothing here is verified on real hardware.
- It does not close any open defect (OPEN-1/2/12/13, U1–U3).

## 5. Size

P1a adds two small arrays per CPU (100 list heads and a bitmap) and five tcb fields. That is under 2 KB of BSS for 64 CPUs, plus code. The P1a PR measures the `kernel.bin` delta against the 65,142 B of headroom.
