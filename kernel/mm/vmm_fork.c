/* kernel/mm/vmm_fork.c — copy-all-pages address-space fork (Phase 5b slice 8).
 *
 * ADR-022 baseline: fork duplicates every present USER page into a fresh address
 * space (copy-on-write is the IMP-D follow-on). The new AS starts from
 * vmm_new_address_space (which shares the kernel's top-level entries), then we
 * walk the parent's 4-level tables through the low identity map and, for each
 * present user leaf, allocate a zeroed frame, memcpy the parent page into it, and
 * map it in the child with the parent's flags. Frames are ptnode_alloc'd so
 * vmm_destroy_address_space (which ptnode_free's user leaves) reclaims them.
 *
 * The walk constants mirror vmm.c's private definitions (standard x86-64 paging);
 * keeping them local avoids widening vmm.c's API surface for a one-off consumer.
 */
#include "vmm_fork.h"
#include "vmm.h"
#include "pmm.h"          /* PAGE_SIZE */
#include "kheap.h"        /* ptnode_alloc / ptnode_free */
#include "string.h"       /* memcpy */

#define PG_PS    0x80ull                       /* huge-page bit (no descent)     */
#define PG_ADDR  0x000FFFFFFFFFF000ull         /* 4 KiB-aligned phys addr field  */

static inline uint64_t *table_at(uint64_t phys) {
    return (uint64_t *)(uintptr_t)(phys & PG_ADDR);
}

uint64_t vmm_fork_address_space_copy(uint64_t parent_cr3) {
    uint64_t child = vmm_new_address_space();
    if (!child)
        return 0;

    uint64_t *ppml4 = table_at(parent_cr3);
    uint64_t *kpml4 = table_at(vmm_kernel_cr3());

    for (unsigned i4 = 0; i4 < 512; i4++) {
        uint64_t e4 = ppml4[i4];
        if (!(e4 & VMM_PRESENT) || (e4 & PG_PS))
            continue;
        if (e4 == kpml4[i4])
            continue;                          /* shared kernel top-level entry  */
        uint64_t *pdpt = table_at(e4);
        for (unsigned i3 = 0; i3 < 512; i3++) {
            uint64_t e3 = pdpt[i3];
            if (!(e3 & VMM_PRESENT) || (e3 & PG_PS))
                continue;
            uint64_t *pd = table_at(e3);
            for (unsigned i2 = 0; i2 < 512; i2++) {
                uint64_t e2 = pd[i2];
                if (!(e2 & VMM_PRESENT) || (e2 & PG_PS))
                    continue;
                uint64_t *pt = table_at(e2);
                for (unsigned i1 = 0; i1 < 512; i1++) {
                    uint64_t e1 = pt[i1];
                    if (!(e1 & VMM_PRESENT) || !(e1 & VMM_USER))
                        continue;              /* copy only present user pages    */

                    uint64_t va = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) |
                                  ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);
                    /* Preserve RW/USER/PWT/PCD + SW bits (low 12) and NX (bit 63);
                     * map_core re-adds PRESENT. PTE_SW_COW (the future COW bit) is
                     * not set in the copy-all-pages baseline. */
                    uint64_t flags = (e1 & 0xFFFull) | (e1 & VMM_NX);

                    if (e1 & PTE_SW_SHARED) {
                        /* Shared frame (e.g. the vDSO): map the SAME physical page
                         * into the child — never copy or free a shared frame. */
                        if (vmm_map_in(child, va, e1 & PG_ADDR, flags) != 0) {
                            vmm_destroy_address_space(child);
                            return 0;
                        }
                        continue;
                    }

                    void *frame = ptnode_alloc();           /* zeroed child page  */
                    if (!frame) {
                        vmm_destroy_address_space(child);
                        return 0;
                    }
                    memcpy(frame, (const void *)(uintptr_t)(e1 & PG_ADDR),
                           (size_t)PAGE_SIZE);
                    if (vmm_map_in(child, va, (uint64_t)(uintptr_t)frame, flags) != 0) {
                        ptnode_free(frame);
                        vmm_destroy_address_space(child);
                        return 0;
                    }
                }
            }
        }
    }
    return child;
}
