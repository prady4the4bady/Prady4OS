# ADR-006: Interrupt controller — legacy 8259 PIC now, APIC later

- **Status:** Accepted (provisional — APIC supersedes this in Phase 2b)
- **Date:** 2026-06-17
- **Phase:** 2a

## Context

Phase 2a needs a working hardware-interrupt pipeline (a periodic timer is the
prerequisite for preemptive scheduling). The blueprint's component tracker calls
for the APIC. But the APIC (Local APIC + I/O APIC) needs ACPI/MADT parsing (or
MSR/MMIO setup) that does not exist yet, and the goal here is to prove the
interrupt path end-to-end cheaply.

## Decision

Use the **legacy 8259 PIC + 8254 PIT** for now (decision confirmed with the
user):

- Remap the master/slave PICs to vectors 0x20..0x2F (clear of the CPU exception
  range), unmask only IRQ0 (timer) and IRQ1 (keyboard).
- Program PIT channel 0 to 100 Hz (mode 3). IRQ0 increments a global tick count.
- IRQ1 reads the keyboard scancode (port 0x60) and prints it.
- IDT gates extended to 48 (0..31 exceptions, 32..47 IRQs). The IRQ path shares
  the exception trampoline; the C dispatcher branches on vector and sends EOI.

**APIC is explicitly deferred to Phase 2b**, once ACPI/MADT parsing is in place.
At that point the PIC is masked/disabled and replaced by LAPIC + I/O APIC + the
APIC timer.

## Consequences

- Cheap, well-understood, and enough to drive a scheduler tick. Verified: the
  kernel observes the PIT ticking after `sti`.
- **Spurious IRQ7/IRQ15 not specially handled yet.** A spurious IRQ would be
  EOI'd like a real one; correct handling (check the in-service register, skip
  EOI for spurious) is a small follow-up. Low risk in QEMU over the short run.
- No SMP/IRQ affinity (PIC is single-CPU). That is inherent to the PIC and a
  reason the APIC migration matters once we go multi-core.

## Alternatives considered

- **APIC now:** the blueprint's target, but premature without ACPI table parsing;
  larger and riskier for a first interrupt slice. Deferred, not rejected.
