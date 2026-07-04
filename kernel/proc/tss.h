/* kernel/proc/tss.h — 64-bit Task State Segment (Phase 2e; per-CPU ADR-031).
 *
 * In long mode the TSS mainly supplies RSP0: the stack the CPU switches to when
 * an interrupt or exception is taken while running in ring 3. tss_set_rsp0 must
 * point at the current user thread's kernel stack before it runs in ring 3.
 *
 * DDR-SMP-3c-cap-1: one TSS + GDT descriptor + TR PER CPU (indexed by cpu_idx).
 * tss_set_rsp0 targets the calling CPU's own TSS (via this_cpu()).
 */
#pragma once
#include <stdint.h>

/* Fill CPU `cpu_idx`'s TSS, patch its GDT descriptor (selector 0x28+idx*0x10),
 * and LTR it on the calling CPU. Caller must already be on the shared gdt64. */
void tss_init_cpu(uint32_t cpu_idx, uint64_t rsp0);
void tss_set_rsp0(uint64_t rsp0);    /* update THIS CPU's ring-0 stack for ring-3 entry */
