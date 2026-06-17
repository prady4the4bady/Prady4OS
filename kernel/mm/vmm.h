/* kernel/vmm.h — NEXUS virtual memory manager (4-level paging), Phase 2b.
 *
 * The kernel owns its page tables: vmm_map/vmm_unmap walk the active 4-level
 * tables (rooted at CR3), allocating intermediate tables from the PMM as needed
 * and reclaiming emptied tables on unmap. Page-table frames are reached through
 * the low identity map (kept by the bootloader), so no physmap is needed yet.
 */
#pragma once
#include <stdint.h>

#define VMM_PRESENT 0x1ull
#define VMM_RW      0x2ull
#define VMM_USER    0x4ull
#define VMM_NX      0x8000000000000000ull   /* requires EFER.NXE; not set yet */

/* Map/unmap a single 4 KiB page. Returns 0 on success, -1 on failure
 * (e.g. a huge page already covers the address, or out of frames). */
int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
int vmm_unmap(uint64_t virt);
