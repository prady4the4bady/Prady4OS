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
void     pmm_free_pages(uint64_t addr, unsigned order);
uint64_t pmm_alloc_page(void);              /* order 0 */
void     pmm_free_page(uint64_t addr);
uint64_t pmm_free_page_count(void);         /* current free frame count */
