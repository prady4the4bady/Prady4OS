# DDR-SMP-3c-locks-1 — sched spinlock + cross-CPU wake (ADR-030 stage 3c)

> DDR before code. The full-3c prerequisite campaign starts at the scheduler
> itself, and ships the first cross-CPU scheduling primitive: **an AP job can
> wake a blocked BSP thread**.

## Decisions

### D1 — The scheduler's masking becomes the sched spinlock
`sched.c`'s `irq_save/irq_restore` helpers become wrappers over `g_sched_lock`
(`spin_lock_irqsave`), the stage-1 pattern: identical one-CPU semantics, real
mutual exclusion for the coming AP participation. This covers `schedule()`'s
ring walk and the reaper's scan (the existing call sites).

**The handoff subtlety:** `schedule()` holds the lock **across
`context_switch`**; the resuming thread's `irq_restore` releases it (a
test-and-set lock has no owner — this is the classic switch-lock handoff). But
a **newly created** thread's first entry runs `thread_trampoline`, not a
resumed `schedule()` frame: under masking, its crafted initial RFLAGS
auto-"released" via `popfq`; under a lock it would leak the acquisition and
deadlock the next `schedule()`. Fix: `thread_trampoline` releases
`g_sched_lock` as its first act.

### D2 — `sched_unblock` becomes an atomic CAS, callable from any CPU
BLOCKED→READY is a pure state transition — no ring topology change — so an
atomic compare-exchange makes it AP-safe without taking the ring lock: the
BSP's locked walk observes READY either this pass or the next tick (benign).
**Topology mutations (create/destroy/exit) remain BSP-only** — enforced by the
current model (APs run only mailbox jobs); each later slice that gives APs more
must lock what it touches.

### D3 — The capability + proof
A boot-time test: a BSP kernel thread prints `[smp] cross-wake waiting` and
blocks; kmain dispatches an AP job that `sched_unblock`s it; the thread resumes
on the BSP and prints `[smp] cross-wake OK`. This is the first time one CPU
schedules work observed by another — the seed of reschedule IPIs.

### D5 — Root-cause fix folded in: the proof must run under the scheduler
The first placement of the proof — kmain's APIC section — predates
`sched_init` (`sched_demo()` is kmain's *last* line; every later phase runs as
a scheduled thread). `sched_create` with a NULL `current_thread` silently
corrupted through the identity-mapped page 0 (no fault — the DDR-713 "verify
the timeline first" lesson again, diagnosed by bisecting GS-base reads that
turned out fine). The proof now runs from the scheduled FS-phase thread,
gated on a `g_smp_have_aps` flag set at bring-up.

## Gate
`smoke-crosswake` (`-smp 4`): both sentinels. 58 CI gates total.

## Non-goals
Locked ring topology for APs; per-CPU runqueues/TSS/idle; preemption IPIs;
wait4/pipe/epoll waiter-field auditing (needed before APs run *blocking*
kernel work — next slices).
