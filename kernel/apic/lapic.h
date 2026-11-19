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

/* Software-enable the calling AP's LAPIC (SVR + TPR) so it can receive the
 * wake IPI (DDR-SMP-3c-alpha). The MMIO window is per-CPU at the same base. */
void lapic_ap_enable(void);

/* cap-3: arm the calling CPU's LAPIC timer at the BSP-calibrated 100 Hz, so its
 * vector-48 tick drives sched_tick -> schedule() (per-AP preemption). */
void lapic_timer_ap_arm(void);

#define LAPIC_WAKE_VECTOR (LAPIC_TIMER_VECTOR + 1)   /* 49: AP wake IPI */

/* CPU count from the MADT (type-0 entries) — reported now, used by stage B. */
unsigned lapic_cpu_count(void);

/* This CPU's LAPIC id (the BSP's, when called from kmain). */
uint32_t lapic_id(void);

/* The i-th MADT processor entry's LAPIC id (i < lapic_cpu_count()). */
uint32_t lapic_apic_id_at(unsigned i);

/* Send an IPI: dest LAPIC id + the ICR low word (delivery mode/vector/level),
 * then wait for the delivery-status bit to clear (ADR-029 INIT-SIPI). */
void lapic_send_ipi(uint32_t dest_id, uint32_t icr_low);

/* DDR-981: fire an NMI at one LAPIC id — reaches a CPU that is not taking its
 * ordinary interrupts, which is the whole point of the AP-freeze probe. */
void lapic_send_nmi(uint32_t dest_id);

/* DDR-981: the calling CPU's own LVT-timer / TPR / SVR, plus the in-service and
 * request bits for LAPIC_TIMER_VECTOR. Safe from NMI context (MMIO reads only,
 * no locks). All-ones in every field when the LAPIC is not mapped. */
void lapic_snapshot(uint32_t *lvt, uint32_t *tpr, uint32_t *svr,
                    uint32_t *isr48, uint32_t *irr48);

/* DDR-1188 Amendment 3 (C1.5 P2): one-shot timers on the LAPIC count.
 *
 * A CPU's timer stays periodic unless a one-shot is pending before its next
 * tick (T2). hrt_arm asks for a wake of `t` at `deadline_ns` (CLOCK_MONOTONIC)
 * on the calling CPU; it returns 1 when a slot was taken and 0 when the sleep
 * must rely on the tick (table full, budget spent, no LAPIC). The wake goes
 * through futex_kick, so `t` must register its wait lock in t->fx_lock.
 *
 * Lock order (T3): the per-CPU table lock, then the wait lock. Call hrt_arm
 * before taking the wait lock and hrt_cancel after dropping it, and cancel on
 * every way out of the wait: a slot must never outlive its sleep.
 *
 * hrt_irq is called by the timer vector after the EOI. It returns 1 when this
 * interrupt is the periodic tick and 0 when it was an early one-shot. */
struct tcb;
int  hrt_arm(struct tcb *t, uint64_t deadline_ns);
void hrt_cancel(struct tcb *t);
int  hrt_irq(void);

/* DDR-1188 Amendment 7 (C1.5 P6): an idle AP's bounded tick stop. Own CPU,
 * interrupts off. hrt_nohz_enter programs one one-shot of NOHZ_MAX_TICKS and
 * returns 1, or returns 0 and changes nothing when a one-shot slot is pending,
 * the timer is not periodic, or a timer interrupt is already latched (T2 b).
 * While stopped, hrt_irq answers 0 (not a tick). hrt_nohz_exit restores the
 * periodic tick and returns the whole ticks elapsed since entry (T4). */
#define NOHZ_MAX_TICKS 100u
int      hrt_nohz_enter(void);
uint64_t hrt_nohz_exit(void);
