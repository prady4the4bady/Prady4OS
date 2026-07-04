# ADR-031: APs enter the scheduler — per-CPU TSS/idle, locked topology, preemption

- **Status:** Accepted
- **Date:** 2026-07-05
- **Realizes:** ADR-030 stages 3–4 (planned there, decided here)
- **Supersedes:** the "topology mutations remain BSP-only" restriction of
  DDR-SMP-3c-locks-1 §D2; the remaining ADR-016 masking on the scheduler.
  Binding until superseded by a later ADR.

## Context

ADR-030 brought the APs online and staged the lock migration; the subsystem
locks are now complete and CI-green — the scheduler ring (locks-1, `g_sched_lock`
+ atomic `sched_unblock`), the block layer (locks-2), VFS (locks-3), and IPC +
the broadcast bus (locks-4, with the `sched_block_on` sleep primitive). Every
shared subsystem an AP could touch is cross-CPU safe. What remains is the payoff:
the APs stop parking in their mailbox `hlt` loop and **run threads**.

The one contract in the way is DDR-SMP-3c-locks-1 §D2, which kept topology
mutations (create / destroy / exit / reap) BSP-only because only the BSP ran
threads. Once an AP runs a thread, that thread can `exit` and new threads can be
created from any CPU — topology mutation moves off the BSP. This ADR removes
that restriction under a defined locking discipline, and adds the per-CPU state
a running AP needs. It stays staged (each sub-slice CI-green before the next) to
keep all 58 gates behind small changes.

## Decision

### D1 — Per-CPU TSS / GDT descriptor / TR (cap-1, contract-neutral)
The single global `tss` + single `gdt64_tss[2]` descriptor + single `LTR`
become per-CPU. Each logical CPU has its own TR (SDM vol. 3 §7.2.1: TR is
per-logical-processor), its own TSS, and its own GDT TSS-descriptor slot; the
CPU consults *its own* TSS.RSP0 on a CPL3→CPL0 transition. A shared `RSP0`
would race — two CPUs entering the kernel from ring 3 would read one stack
pointer. `tss_set_rsp0` becomes "set **this CPU's** RSP0" (indexed by
`this_cpu()->cpu_idx`). The BSP path is unchanged (it is just CPU 0).

### D2 — Per-CPU idle thread; the park loop becomes the idle loop (cap-2)
Each CPU gets an idle thread created by the BSP at boot (topology, done before
APs schedule — safe). An AP's `percpu.current` is set to its idle thread, then
the AP calls `schedule()` instead of the mailbox park loop. Idle is a
`hlt`-then-`schedule()` loop that is never BLOCKED and never in the ready ring
(picked only when nothing else is runnable). `smp_run_on`/the job mailbox stay
for targeted BSP→AP calls but no longer gate whether the AP runs the scheduler.

### D3 — Topology mutation under `g_sched_lock`, from any CPU (cap-2, the contract change)
`sched_create`, `sched_destroy`, `sched_exit`, and the reaper scan take
`g_sched_lock` (already held by `schedule()`); DDR-SMP-3c-locks-1 §D2 is
superseded. The exit hazard — a thread must not free the kernel stack it is
still running on — is handled as today: `sched_exit` becomes a ZOMBIE, wakes its
waiter, and switches away **under the lock**; the TCB + address space are
reclaimed by the reaper on another schedule, never in `sched_exit` itself. This
already holds single-CPU; the ADR only widens the lock to cover AP callers. Lock
order: `g_sched_lock` is a leaf over the ring — it is never held across a block
(the switch-lock handoff of locks-1 stands: `schedule()` releases it via the
resumed thread / `thread_trampoline`).

### D4 — Preemption on APs via the per-CPU LAPIC timer (cap-3)
Each LAPIC has its own timer (SDM vol. 3 §10.5.4). Each CPU arms its LAPIC timer
to drive its own `sched_tick`→`schedule()`, so an AP preempts its running thread
without the BSP. Until this slice an AP only reschedules cooperatively (on block
/ yield / idle); D4 makes it preemptive. The existing wake-IPI (vector 49) stays
for directed cross-CPU wakeups.

### D5 — User threads on APs (cap-4, ADR-030 stage 4)
With per-CPU TSS.RSP0 (D1), per-CPU `%gs` `current`/`kstack_top` (DDR-SMP-3b,
already in place), and per-thread CR3/FPU/signal state, a ring-3 thread can run
on any CPU. This slice audits the SYSCALL entry path for per-CPU correctness and
adds the capstone gate: a user thread provably executes on a non-BSP CPU.

## Consequences

- ADR-016 is fully superseded for the scheduler once D3+D4 land; interrupt
  masking remains only where a subsystem still relies on it locally.
- Per-CPU TSS costs one TSS + one GDT descriptor slot per CPU (`PERCPU_MAX`=16);
  the GDT gains descriptor room or each CPU points its TR at a per-CPU GDT.
- Migrating a thread between CPUs is allowed (the ring is shared); FPU/CR3 are
  per-thread so migration is safe. CPU affinity is a non-goal here.
- True contention on `g_sched_lock` now occurs; it is a short leaf critical
  section (ring walk), acceptable. Per-CPU runqueues are a later optimization.

## Sub-slices (each its own DDR, CI-green before the next)
- **cap-1** — per-CPU TSS/GDT/TR (D1). Gate: existing `smoke-smp*` re-verify;
  no behavior change (BSP is CPU 0).
- **cap-2** — per-CPU idle + locked topology; APs enter `schedule()`
  cooperatively (D2, D3).
- **cap-3** — per-CPU LAPIC-timer preemption (D4).
- **cap-4** — user threads on APs + capstone gate (D5).

## Gate
Each sub-slice re-verifies the 58 gates; cap-4 adds a gate asserting a thread
runs on a non-BSP CPU (`-smp 4`). Total grows by one at cap-4.
