/* kernel/irq.h — legacy 8259 PIC + PIT timer (Phase 2a).
 *
 * Provisional: this is the simple, cheap path to a working interrupt pipeline.
 * The blueprint targets the APIC; that replaces this in Phase 2b once ACPI/MADT
 * parsing exists (see ADR-006).
 */
#pragma once
#include <stdint.h>

/* Remap the master/slave PICs to vectors 0x20..0x2F (clear of CPU exceptions),
 * unmask only IRQ0 (timer) and IRQ1 (keyboard). */
void pic_remap(void);

/* Signal end-of-interrupt for a delivered IRQ vector (0x20..0x2F). */
void pic_eoi(uint64_t vector);

/* Unmask a single IRQ line (and the cascade for slave IRQs 8..15). */
void pic_unmask(unsigned irq);

/* Program PIT channel 0 to fire IRQ0 at `hz` Hz (square wave, mode 3). */
void pit_init(uint32_t hz);

/* Incremented by the IRQ0 handler; read by the kernel to observe time passing. */
extern volatile uint64_t g_ticks;
