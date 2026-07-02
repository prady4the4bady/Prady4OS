/* kernel/apic/lapic.h — Local APIC bring-up + APIC timer (DDR-714 stage A).
 *
 * MADT-discovered xAPIC MMIO. Stage A enables the BSP's LAPIC and moves the
 * 100 Hz scheduler tick from the PIT to the APIC timer (vector 48); the 8259
 * stays for device IRQs. Stage B (SMP) builds on this (INIT-SIPI IPIs).
 */
#pragma once
#include <stdint.h>

#define LAPIC_TIMER_VECTOR 48   /* outside the PIC's 32..47 range */

/* Parse the MADT, map + software-enable the BSP LAPIC. Returns 0, or -1 when
 * no MADT/LAPIC is present (the PIT then stays the timer source). */
int  lapic_init(void);

/* Calibrate the APIC timer against the (still-running) PIT and start it
 * periodic at 100 Hz on LAPIC_TIMER_VECTOR, then mask PIT IRQ0 at the PIC.
 * Requires lapic_init() == 0 and interrupts enabled. */
void lapic_timer_100hz(void);

/* Signal end-of-interrupt to the LAPIC (vector-48 tick path in idt.c). */
void lapic_eoi(void);

/* CPU count from the MADT (type-0 entries) — reported now, used by stage B. */
unsigned lapic_cpu_count(void);
