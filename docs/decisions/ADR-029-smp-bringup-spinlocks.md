# ADR-029: SMP bring-up (INIT-SIPI) + spinlock primitive — APs parked

- **Status:** Accepted
- **Date:** 2026-07-02
- **Phase:** DDR-714 stage B

## Context

Stage A (DDR-714) put the LAPIC + APIC timer in charge of the BSP tick. The MADT
already enumerates every CPU. ADR-016 made shared kernel state preemption-safe
with **interrupt masking**, which is explicitly single-core-only: a second CPU
executing kernel code concurrently would race the PMM/console/scheduler no matter
what the BSP's IF says. Full SMP scheduling therefore needs cross-CPU locking in
every shared subsystem — too much blast radius for one slice while 46 gates sit on
those paths.

## Decision

Bring the APs up **parked**, and introduce the spinlock primitive they need:

1. **AP boot** — the classic INIT–SIPI–SIPI sequence (Intel SDM vol. 3 §8.4 /
   MP init protocol) from the BSP's LAPIC (ICR at 0x300/0x310). A 16-bit
   trampoline blob (`arch/x86_64/ap_boot.asm`, linked into the kernel and copied
   to physical **0x8000** = SIPI vector 0x08) takes each AP real mode → PAE+LME →
   long mode directly (SDM 10.8.5), using the **boot page tables** (the kernel
   master CR3) and a per-AP stack + entry mailbox the BSP fills in before the
   SIPI.
2. **Parked, interrupts off.** `smp_ap_main` records the CPU online (atomic),
   announces itself once, and halts (`cli; hlt` loop). APs run **no scheduler, no
   IRQs, no user code** — so ADR-016's masking remains sufficient for every
   existing kernel path, and all 46 gates keep their single-CPU determinism.
3. **Spinlock primitive** — `kernel/include/spinlock.h`: a test-and-set spinlock
   (`__atomic_test_and_set` / `pause` / release store) with an irq-save variant.
   Stage B uses it only to serialize the APs' boot announcements against each
   other. It is the building block the follow-on ADR will apply to
   PMM/console/scheduler when APs start doing real work.
4. **Scheduling stays BSP-only.** Distributing the scheduler (per-CPU run
   state, cross-CPU locking of PMM/kheap/console/sched, TLB shootdowns, per-CPU
   TSS/GDT) is a **future ADR** that supersedes ADR-016; until then ADR-016
   stands, scoped to the single scheduling CPU.

## Consequences

- `[smp] cpus online=N/N` proves real multi-core bring-up end-to-end (MADT →
  IPIs → trampoline → long mode → C) — the hard, risky part of SMP — without
  destabilizing the kernel's single-core concurrency contract.
- Parked APs burn no meaningful power (`hlt`) and cannot corrupt anything (no
  interrupts, no shared-state writes beyond the online flag + guarded print).
- The 0x8000 physical page is reserved at boot for the trampoline (below the
  kernel at 0x10000; nothing else uses it).
- QEMU gates run `-smp 1` by default (no APs — `online=1/1`); the new
  `smoke-smp` gate boots `-smp 4` via a `QEMU_SMP` runner knob.

## Alternatives considered

- **Full SMP scheduling now:** rejected — requires relocking every shared
  subsystem at once; staged bring-up isolates the (independently risky) MP-init
  protocol first.
- **x2APIC (MSR) IPIs:** xAPIC MMIO is already mapped and sufficient for q35 and
  period hardware; x2APIC deferred.
