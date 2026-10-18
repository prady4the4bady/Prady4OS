/* kernel/mm/tlb.h — IPI-based cross-CPU TLB shootdown (DDR-1177, checklist C1.2).
 *
 * The caller edits the PTEs and flushes its OWN TLB first (existing invlpg
 * sites). tlb_shoot() then edits nothing: it publishes one request, sends the
 * vector-0xF0 IPI to every CPU that may hold a stale entry, and returns only
 * when each has acked. Frames and page-table nodes may be freed only AFTER it
 * returns.
 *
 * Status: gated by smoke-tlbshoot (arms A-C, kernel/mm/tlb_probe.c).
 */
#pragma once
#include <stdint.h>

#define TLB_SHOOT_VECTOR     0xF0u   /* DDR-1177 §3.2: class 15, above timer/MSI-X */
#define TLB_SHOOT_INVLPG_MAX 32u     /* DDR-1177 §3.6: placeholder, NOT measured   */
#define TLB_FULL             0u      /* npages value meaning "full flush"          */

/* cr3 = the AS to flush (any value with the low 12 bits ignored; the kernel
 * master's root also counts as a user AS here), or 0 = a kernel-half change,
 * which targets every ready CPU. va/npages = the range; npages == TLB_FULL or
 * > TLB_SHOOT_INVLPG_MAX makes targets reload CR3 instead. No-op on one CPU. */
void tlb_shoot(uint64_t cr3, uint64_t va, uint64_t npages);

/* Vector-0xF0 body, called from isr_dispatch (which EOIs). */
void tlb_shoot_isr(void);

/* Mark the calling CPU able to take the shootdown IPI (IDT loaded, LAPIC
 * software-enabled). Only ready CPUs are ever targeted. */
void tlb_cpu_ready(void);

/* smoke-tlbshoot only: the last published generation and its target mask. */
void tlb_probe_last(uint64_t *gen, uint64_t *mask);

/* Diagnostic counters (relaxed). */
extern volatile uint64_t g_tlb_shoot_reqs, g_tlb_shoot_ipis;
