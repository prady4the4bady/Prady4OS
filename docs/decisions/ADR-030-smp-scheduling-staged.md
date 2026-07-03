# ADR-030: Distributed SMP scheduling — staged; stage 1: subsystem spinlocks

- **Status:** Accepted (stage 1); stages 2–4 planned
- **Date:** 2026-07-03
- **Supersedes:** ADR-016 progressively (per stage), using ADR-029's spinlock

## Context

ADR-029 brought the APs online **parked**. Letting them schedule real threads
requires every shared kernel subsystem to be safe against true concurrency —
ADR-016's interrupt masking only serializes one CPU. Flipping everything at once
would put all 52 gates at risk behind a single enormous change, so the migration
is staged, each stage CI-green before the next:

1. **Stage 1 (this ADR/slice): lock the foundational subsystems.**
   PMM, kheap, and the console move from interrupt masking to
   `spin_lock_irqsave` (ADR-029's primitive — the irqsave variant preserves
   ADR-016's single-CPU semantics exactly, adding cross-CPU mutual exclusion).
   Notably the **kheap slab lists had no masking at all** — safe only by the
   call pattern (no allocation in IRQ paths); stage 1 closes that latent class
   too by locking every public kheap entry point. Each AP proves the locked
   paths from a second CPU at boot: `pmm_alloc/free` + `kmalloc/kfree` before
   parking (`[smp] cpu N locks OK`). True *contention* (concurrent CPUs inside
   the allocators) arrives with stage 3's scheduling; stage 1 proves
   correctness of the lock plumbing from non-BSP CPUs.
2. **Stage 2: per-CPU infrastructure.** GS-based per-CPU area
   (`current_thread`, syscall kstack top), per-CPU TSS/GDT, per-CPU LAPIC
   timer. No scheduling change yet.
3. **Stage 3: the scheduler ring under its lock; APs run kernel threads.**
   Idle threads per CPU; reschedule IPIs; ADR-016 fully superseded.
4. **Stage 4: user threads on APs.** Per-CPU SYSCALL entry state, CR3/FPU/
   signal delivery audited per CPU; the fork/exec/wait paths under test.

## Decision (stage 1)

- `kernel/mm/pmm.c` and `kernel/console.c`: the existing local
  `irq_save()/irq_restore()` helpers become wrappers over a per-subsystem
  spinlock's irqsave acquire/release — call sites unchanged, semantics on one
  CPU identical, cross-CPU exclusion added.
- `kernel/mm/kheap.c`: a single heap lock taken in every public entry point
  (`kmalloc/kfree`, the pcb/cap/ipc pools, `ptnode_alloc/free`,
  `kheap_outstanding`). Lock ordering: kheap → PMM (kheap grows slabs from the
  PMM); the PMM never calls kheap, so no cycle.
- `smp_ap_entry` exercises both allocators before parking and reports.

## Consequences

- Lock overhead on hot paths is one uncontended atomic per operation on a
  single CPU — noise against QEMU TCG variance; all 52 gates re-verify it.
- The console lock nests inside PMM/kheap critical sections only via panic
  paths (prints while holding a lock) — acceptable: panics halt anyway.
- Scheduler, VFS, block layer, IPC, AETHER rings remain ADR-016-masked until
  their stage; APs still run no scheduler and take no IRQs, so that is sound.

## Gate
`smoke-smplock` (`-smp 4`): all three APs report `locks OK` + `cpus online=4/4`.
53 CI gates total.
