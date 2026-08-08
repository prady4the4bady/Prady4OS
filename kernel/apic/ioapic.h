/* kernel/apic/ioapic.h — I/O APIC + q35 GSI routing (Group 4 item 28, DDR-874).
 *
 * The 8259 PIC (kernel/irq.c) routes every legacy IRQ to the BSP and nowhere
 * else. The I/O APIC replaces that with a redirection table: each Global System
 * Interrupt (GSI) gets its own entry naming a vector and a destination LAPIC,
 * which is what makes per-CPU interrupt affinity possible at all.
 */
#pragma once
#include <stdint.h>

/* Parse the MADT, map the I/O APIC, mask every input. Returns 1 when an I/O
 * APIC was found and initialised, 0 when the platform has none (the PIC then
 * stays in charge). */
int  ioapic_init(void);

/* Route a GSI to `vector` on the LAPIC with `apic_id`, applying any MADT
 * Interrupt Source Override for the legacy IRQ. Returns 0 on success. */
int  ioapic_route(uint8_t irq, uint8_t vector, uint8_t apic_id);

void ioapic_mask(uint8_t irq);
void ioapic_unmask(uint8_t irq);

/* Diagnostics for the gate: how many redirection entries the chip reports, and
 * the GSI a legacy IRQ actually lands on after overrides. */
unsigned ioapic_redir_count(void);
uint32_t ioapic_gsi_for_irq(uint8_t irq);
int      ioapic_available(void);
