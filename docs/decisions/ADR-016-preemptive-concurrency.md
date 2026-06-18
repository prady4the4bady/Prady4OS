# ADR-016: Preemptive-safe shared state (PMM, console, scheduler)

- **Status:** Accepted 2026-06-18
- **Phase:** 4 (slice 4c — surfaced while bringing up concurrent FS + block I/O)

## Context

Phase 2c introduced preemptive multitasking, but the early subsystems were
written for the single-threaded bring-up era and mutate global state with no
mutual exclusion. With multiple threads now running concurrently (the ring-3
demo thread, the block self-test, and the FAT32 thread), these are latent
correctness bugs that must be fixed before relying on concurrent kernel threads.

> Note on discovery: these gaps were found while chasing an intermittent FS
> test failure. That failure's **actual** root cause turned out to be unrelated
> — a disk-image overlap bug in the block self-test (it wrote a scratch pattern
> to a boot-disk sector that the grown kernel image now occupies, and QEMU
> persisted it, corrupting the kernel for the next boot; see
> `docs/test_results/phase_4.md`). The three fixes below are nonetheless real
> preemptive-concurrency defects and are fixed proactively.

The system is **single-core** (no SMP/APIC yet), so masking interrupts around a
critical section is sufficient mutual exclusion: nothing else can run on this
CPU while interrupts are off. A real spinlock arrives with SMP (tracked).

## Decision

Make the three contended subsystems interrupt-atomic, saving and restoring the
caller's interrupt flag (so a critical section entered with interrupts already
masked stays masked on return — no accidental re-enable):

1. **Buddy PMM** (`kernel/mm/pmm.c`): `pmm_alloc_pages` / `pmm_free_pages` wrap
   the free-list manipulation in `irq_save`/`irq_restore`. Without this, two
   threads allocating concurrently could pop the same block (double allocation)
   or corrupt the intrusive free lists.
2. **Console** (`kernel/console.c`): `kputs` / `kputhex` / `kputdec` emit their
   whole string as one interrupt-masked unit, so concurrent threads no longer
   interleave mid-line. (`kputc` stays the raw primitive for IRQ-context echo.)
3. **Scheduler** (`kernel/proc/sched.c`): `schedule()` runs with interrupts
   masked end to end. It is reached both voluntarily (`yield`/`sched_block`) and
   from the timer IRQ (`sched_tick`); a voluntary `schedule()` that ran with
   interrupts enabled (e.g. the `yield()` in the virtio-blk driver's per-device
   busy-wait) could be **re-entered by a timer tick mid-context-switch**,
   corrupting thread state. Per-thread interrupt state is still preserved across
   the switch by `context_switch` (pushfq/popfq), so masking inside `schedule()`
   does not leak IF between threads.

## Consequences / deferred

- **Single-core assumption.** All three use interrupt masking, valid only
  because one CPU runs at a time. SMP needs real spinlocks (deferred with APIC).
- Interrupt-masked windows are short (a list splice, one line of output, one
  context switch), so timer-tick latency impact is negligible at 100 Hz.
- The virtio-blk driver still serialises to **one request in flight per disk**
  (ADR-014/015); fully concurrent in-flight I/O (a request-tag table) remains
  deferred. What 4c fixes is that concurrent *callers* and the scheduler no
  longer corrupt each other — not that a single disk pipelines requests.

## Verification

With these fixes (and the separate disk-overlap fix that was the real cause of
the test failure), the FAT32 read-write self-test runs concurrently with the
block round-trip test cleanly and repeatably: `make smoke-fs` passes across many
back-to-back runs, `make smoke` (kernel gate) and `make smoke-fs-rw` (write +
host `fsck.fat`) pass, and concurrent thread output no longer interleaves
mid-line. Warning-free `-Werror` build.
