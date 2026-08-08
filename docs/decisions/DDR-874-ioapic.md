# DDR-874 — I/O APIC + q35 GSI routing (Group 4 item 28)

**Status:** Accepted
**Date:** 2026-08-09
**Scope:** `kernel/apic/ioapic.{c,h}`, `kernel/main.c`, Makefile.

## What this adds

The 8259 PIC delivers every legacy IRQ to the BSP, full stop. The I/O APIC has a
redirection table instead — one entry per Global System Interrupt, each naming a
vector **and a destination LAPIC**. That is the mechanism per-CPU interrupt
affinity needs; without it, "route this device to that core" has nowhere to be
expressed.

This lands the table. It does **not** move existing IRQs off the PIC: the PIC
keeps working exactly as before, and `ioapic_route()` is the opt-in path. Moving
the timer is a scheduling change with its own blast radius and belongs with
item 37 (per-CPU affinity), not here.

## The part that is easy to get wrong

**ISA IRQ *n* does not reliably equal GSI *n*.** On q35 the PIT is the standard
case: firmware routes IRQ 0 to GSI 2 and says so with a MADT Interrupt Source
Override. A driver that programs redirection entry 0 for the timer on such a
board arms an input nothing is wired to, receives no interrupts, and looks
exactly like a dead timer.

So the override table is parsed first and **every** route goes through
`ioapic_gsi_for_irq()`. There is no path in this file that uses a raw IRQ number
as a GSI index.

This is not defensive theatre — the boot output shows **5 overrides** present on
the machine the gates actually run on.

Polarity and trigger mode come from the override flags too. ISA defaults are
active-high edge-triggered; a PCI line arriving via an override is typically
active-low level-triggered, and programming the ISA default for it would either
miss the edge or never de-assert.

**Masked by default.** Every entry is masked at init and unmasked only by an
explicit route. Inheriting whatever firmware left armed means taking interrupts
for devices no driver has claimed, with no handler installed.

**The MADT is walked by its length field**, not by per-type sizes — trusting a
fixed size walks off the end the moment firmware emits a longer revision of an
entry, and a malformed entry stops the walk rather than being guessed at.

## The bug this hit, worth recording

First run faulted in `ioapic_init` with a register dump. Cause: the MADT gives a
**physical** address, and `0xFEC00000` is high MMIO that the boot identity map
does not cover. The ACPI tables themselves happen to live in identity-mapped low
RAM, which makes it very easy to assume the same of the I/O APIC — it is not.

Fixed by identity-mapping the page `VMM_RW | VMM_PCD`, matching what `lapic.c`
does for `0xFEE00000`. **PCD is not optional**: an MMIO register file must never
be cached, or a write can sit in a cache line instead of reaching the chip.

## Verification

Detection is real, not a silent no-op returning 0:

```
[ioapic] gsi_base=0 redirs=24 overrides=5
```

24 redirection entries and 5 Interrupt Source Overrides — the expected q35
profile, and confirmation that the override path is exercised rather than
hypothetical.

Gates green: `smoke`, `smoke-apic`, `smoke-smp`. Zero warnings under `-Werror`.

Platforms without an I/O APIC (or without a MADT) log the reason and return 0,
leaving the PIC in charge.

**Group 4 item 28 complete.**
