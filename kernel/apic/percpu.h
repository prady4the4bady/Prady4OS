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

struct tcb;

struct percpu {
    struct percpu *self;   /* @0: this_cpu() reads %gs:0 (DDR-SMP-3a)            */
    struct tcb *current;   /* @8: this CPU's running thread (DDR-SMP-3b)         */
    uint64_t kstack_top;   /* @16: SYSCALL stack switch — asm reads [gs:16]      */
    void (*job)(void);     /* @24: single-slot work mailbox (DDR-SMP-3c-alpha)   */
    uint32_t cpu_idx;      /* MADT roster index (0 = BSP)                        */
    uint32_t apic_id;      /* this CPU's LAPIC id                                */
    uint8_t  present;
    uint8_t  is_bsp;       /* 1 on the bootstrap processor (cap-2b D4); travels
                            * through the percpu_init_bsp migration copy         */
};

/* Another CPU's entry by roster index (the BSP writes an AP's mailbox). */
struct percpu *percpu_get(uint32_t cpu_idx);

/* Claim slot 0 for the BSP before the scheduler's first tick (kmain top);
 * percpu_init_bsp later fills the LAPIC id (migrating slots if the BSP's
 * roster index isn't 0). DDR-SMP-3b D3. */
void percpu_init_early(void);

/* Record the calling CPU's identity (BSP after lapic_init; APs in smp_ap_entry). */
void percpu_init_cpu(uint32_t cpu_idx);

/* BSP convenience: find its MADT roster index by LAPIC id and record it. */
void percpu_init_bsp(void);

/* The calling CPU's percpu entry, resolved by LAPIC id; NULL before init. */
struct percpu *this_cpu(void);
