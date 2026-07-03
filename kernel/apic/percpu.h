/* kernel/apic/percpu.h — per-CPU identity area (ADR-030 stage 2, DDR-SMP-2).
 *
 * LAPIC-ID-indexed (NOT %gs-based: without SWAPGS discipline a ring-3 gs
 * selector reload could clobber a MSR-set base and break the kernel — see the
 * DDR). this_cpu() is safe from any context on any CPU. Fields grow with the
 * ADR-030 stage that uses them.
 */
#pragma once
#include <stdint.h>

#define PERCPU_MAX 16

struct percpu {
    uint32_t cpu_idx;    /* MADT roster index (0 = BSP)  */
    uint32_t apic_id;    /* this CPU's LAPIC id           */
    uint8_t  present;
};

/* Record the calling CPU's identity (BSP after lapic_init; APs in smp_ap_entry). */
void percpu_init_cpu(uint32_t cpu_idx);

/* BSP convenience: find its MADT roster index by LAPIC id and record it. */
void percpu_init_bsp(void);

/* The calling CPU's percpu entry, resolved by LAPIC id; NULL before init. */
struct percpu *this_cpu(void);
