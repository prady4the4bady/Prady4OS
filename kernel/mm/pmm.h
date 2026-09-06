/* kernel/pmm.h — NEXUS physical memory manager (buddy allocator), Phase 2b.
 *
 * Manages physical page frames as power-of-2 blocks (orders 0..MAX_ORDER-1,
 * block size = 4 KiB << order). Seeded from the E820 map in the boot_info.
 * Decision: ADR-003 (buddy now; the Blueprint's PFO coalescing is a later,
 * measurement-gated enhancement).
 *
 * Provisional bound: only RAM in [16 MiB, 1 GiB) is managed for now, because
 * (a) the low 16 MiB holds the kernel/bootloader/page-tables/BIOS regions and
 * (b) only the low 1 GiB is identity-mapped (ADR-005), and the intrusive free
 * lists store their links inside the free frames themselves. Lifts when the
 * Phase 2b VMM maps more.
 */
#pragma once
#include <stdint.h>
#include "boot_info.h"

#define PAGE_SIZE  4096u
#define PAGE_SHIFT 12u
#define PMM_MAX_ORDER 11u            /* orders 0..10; largest block = 4 MiB */

void     pmm_init(const struct boot_info *bi);
uint64_t pmm_alloc_pages(unsigned order);   /* physical addr, or 0 on failure */

/* DDR-882 (item 17b): allocate preferring `node`. Falls back to other nodes
 * rather than failing — a node is a locality PREFERENCE, not a constraint. */
uint64_t pmm_alloc_pages_node(unsigned node, unsigned order);

/* Re-file every free block onto its owning node's list. Called once, after
 * numa_init(), because pmm_init() runs before ACPI is readable. */
void     pmm_numa_rebucket(void);
int      pmm_free_pages(uint64_t addr, unsigned order);  /* DDR-1065: 1 = released, 0 = ref dropped */
uint64_t pmm_alloc_page(void);              /* order 0 */
int      pmm_free_page(uint64_t addr);      /* drops one reference; frees at 0 (IMP-D).
                                             * DDR-1065: 1 = released, 0 = only dereferenced */
uint64_t pmm_free_page_count(void);         /* current free frame count */
uint64_t pmm_total_page_count(void);        /* total managed frames (DDR-752) */

/* Copy-on-write reference counting (IMP-D). Each managed frame has a 16-bit
 * refcount: alloc sets it to 1, pmm_free_page decrements and frees only at 0.
 * COW fork calls pmm_incref when sharing a frame; the #PF handler reads the
 * count to decide copy-vs-regrant. */
void     pmm_incref(uint64_t phys);
uint16_t pmm_refcount_get(uint64_t phys);
