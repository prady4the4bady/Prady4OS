# DDR-714 — APIC + SMP (staged; stage A: LAPIC + APIC timer)

> DDR before code. ADR-006 chose the legacy 8259 PIC + 8254 PIT "now" and
> explicitly deferred the APIC until ACPI parsing existed (it has since Phase 3).
> This DDR begins that migration. **APIC+SMP is the largest blast-radius change in
> the kernel** — all 45 gates sit on the interrupt path — so it ships in stages,
> each CI-green before the next starts (the slice rule).

## Stages

- **A (this slice):** MADT parse + **Local APIC** enable on the BSP + **APIC
  timer** takes over the 100 Hz scheduler tick (PIT masked). The 8259 PIC stays
  for device IRQs (keyboard, COM1 RX, PCI INTx) — the "hybrid virtual-wire"
  arrangement. This is the SMP prerequisite: INIT-SIPI IPIs and per-CPU timers
  need only the LAPIC, not the I/O APIC.
- **B (done — see ADR-029):** SMP bring-up — AP trampoline
  (`arch/x86_64/ap_boot.asm`, INIT-SIPI-SIPI, real→long mode at 0x8000) brings
  every MADT AP online **parked** (`cli/hlt`, no scheduler/IRQs), plus the
  spinlock primitive (`kernel/include/spinlock.h`). ADR-016 remains valid for
  the single scheduling CPU; distributing the scheduler (per-CPU state +
  locking every shared subsystem) is a future ADR. Gate `smoke-smp`
  (`-smp 4` → `[smp] cpus online=4/4`).
- **C (later):** I/O APIC migration of device IRQs (GSIs, interrupt source
  overrides) + MSI-X for virtio (unblocks the deferred multi-request virtio).

## Stage-A decisions

### D1 — MADT discovery, identity-mapped LAPIC MMIO
`kernel/apic/lapic.c` (new subsystem dir) parses the MADT (`acpi_find_table("APIC")`)
for the LAPIC physical base (typically `0xFEE00000`) and counts type-0
(processor-LAPIC) entries — reported now, used by stage B. The 4 KiB LAPIC page
is identity-mapped **uncached** (`vmm_map(base, base, VMM_RW|VMM_PCD)`), the same
pattern as the PCIe ECAM window. The mapping lands in the shared boot PDPT
(PML4[0] subtree), so every address space sees it — kernel-only (no `VMM_USER`).
No MADT → `[apic] absent — PIT retained` and the PIT keeps ticking (QEMU always
has one; real fallback).

### D2 — LAPIC enable (software), spurious vector
`lapic_init` verifies `IA32_APIC_BASE` (MSR 0x1B) has the global-enable bit (set
by hardware default; we do not relocate the base), then software-enables via the
Spurious Interrupt Vector Register (offset 0xF0): vector 0xFF + APIC-enable
(bit 8), and clears TPR (0x80) so no interrupt class is masked. (Intel SDM
vol. 3, §11.4/11.9 semantics; register offsets are the architectural xAPIC MMIO
layout.)

### D3 — APIC timer takes the tick on a NEW vector 48
The APIC timer is calibrated against the still-running PIT (count-down from
0xFFFFFFFF over 10 PIT ticks = 100 ms, divider 16), then programmed **periodic**
at 100 Hz on **vector 48** — outside the PIC's 32..47 range, so the two timer
sources can never alias. `isr.asm` grows one stub (48) and `idt.c` installs it;
the tick body (g_ticks, vDSO wall clock, `sched_tick`, lwIP poll, signal
delivery) is extracted into a shared helper used by both the legacy IRQ0 path
(PIT + `pic_eoi`) and the new vector-48 path (APIC + `lapic_eoi`, offset 0xB0).
EOI-before-body is preserved (sched_tick may switch away). Once the APIC timer
is armed, **IRQ0 is masked at the PIC** (new `pic_mask`), so exactly one timer
drives the system. Everything downstream (scheduler quantum, vDSO clock rate,
lwIP cadence, signal delivery) is unchanged at 100 Hz.

### D4 — What stage A does NOT change
Device IRQs (1=kbd, 4=COM1, PCI INTx chains) stay on the 8259 — every FS/net/
input gate keeps its exact interrupt path. No SMP, no spinlocks, no I/O APIC,
no MSI. ADR-006 remains accurate ("PIC for devices"); its timer clause is
superseded by this DDR.

## Gate
`smoke-apic` (CI): boot and grep `[apic] up id=` + `[apic] timer 100Hz (PIT
masked)` — plus the existing `NEXUS KERNEL OK` boot sentinel. Every other gate
regresses the tick implicitly (scheduler, vDSO, net timers, signals all ride it).
46 CI gates total.

## Non-goals (this slice)
AP boot / SMP (stage B + spinlock ADR); I/O APIC + GSI routing + interrupt
source overrides (stage C); MSI-X; TSC-deadline timer mode; x2APIC (MSR-based)
mode — xAPIC MMIO is sufficient for QEMU q35 and period hardware.
