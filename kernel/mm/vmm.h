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
#define VMM_PWT     0x8ull                    /* page write-through            */
#define VMM_PCD     0x10ull                   /* page cache disable (for MMIO) */
#define VMM_NX      0x8000000000000000ull     /* no-execute; honored once EFER.NXE is on */

/* Record the kernel master CR3 and enable EFER.NXE (so VMM_NX is honored).
 * Call once early in kmain, before any per-process address space is created. */
void vmm_init(void);

/* The kernel master page-table root (CR3 value for kernel-only threads). */
uint64_t vmm_kernel_cr3(void);

/* 1 if the CPU supports NX and EFER.NXE was enabled (so VMM_NX / W^X is live). */
int vmm_nx_enabled(void);

/* Map/unmap a single 4 KiB page in the ACTIVE address space. Returns 0 on
 * success, -1 on failure (huge page in the way, or out of frames). */
int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
int vmm_unmap(uint64_t virt);

/* Create a new per-process address space: a fresh PML4 sharing the kernel's
 * top-level entries (low identity + higher-half) but with a private user range.
 * Returns the PML4 physical address (a valid CR3), or 0 on failure. */
uint64_t vmm_new_address_space(void);

/* Map a page into a SPECIFIC address space `pml4_phys` (need not be active). */
int vmm_map_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);

/* Free a per-process address space: its private user page tables and the PML4.
 * The shared kernel entries are left intact. */
void vmm_destroy_address_space(uint64_t pml4_phys);
