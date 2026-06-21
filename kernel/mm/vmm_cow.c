/* kernel/mm/vmm_cow.c — copy-on-write fork + #PF handler (IMP-D).
 *
 * Replaces the copy-all-pages baseline (vmm_fork.c). fork shares every present
 * user page with the child and reference-counts the frame; writable pages are
 * downgraded to read-only and tagged PTE_SW_COW in BOTH address spaces so the
 * first write traps to vmm_cow_fault. The vDSO frame (PTE_SW_SHARED) is shared
 * verbatim, never refcounted or copied (vdso teardown skips it).
 *
 * The 4-level walk constants/helpers mirror vmm.c's private ones — kept local to
 * avoid widening vmm.c's API for this consumer (as in vmm_fork.c before it).
 */
#include "vmm_cow.h"
#include "vmm.h"
#include "pmm.h"          /* PAGE_SIZE, pmm_alloc_page/free_page/incref/refcount_get */
#include "kheap.h"        /* ptnode_alloc */
#include "string.h"       /* memcpy */

#define PG_PS    0x80ull
#define PG_ADDR  0x000FFFFFFFFFF000ull

static inline uint64_t *table_at(uint64_t phys) {
    return (uint64_t *)(uintptr_t)(phys & PG_ADDR);
}
static inline unsigned pte_idx(uint64_t va, unsigned level) {  /* 4=PML4 .. 1=PT */
    return (unsigned)((va >> (12 + 9 * (level - 1))) & 0x1FF);
}
static inline void invlpg(uint64_t va) {
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

/* Pointer to the present 4 KiB leaf PTE for `va` in `cr3`, or NULL. */
static uint64_t *leaf_pte(uint64_t cr3, uint64_t va) {
    uint64_t *t = table_at(cr3);
    uint64_t e = t[pte_idx(va, 4)];
    if (!(e & VMM_PRESENT) || (e & PG_PS)) return 0;
    t = table_at(e);
    e = t[pte_idx(va, 3)];
    if (!(e & VMM_PRESENT) || (e & PG_PS)) return 0;
    t = table_at(e);
    e = t[pte_idx(va, 2)];
    if (!(e & VMM_PRESENT) || (e & PG_PS)) return 0;
    t = table_at(e);
    return &t[pte_idx(va, 1)];
}

uint64_t vmm_fork_address_space_cow(uint64_t parent_cr3) {
    uint64_t child = vmm_new_address_space();
    if (!child)
        return 0;

    uint64_t *ppml4 = table_at(parent_cr3);
    uint64_t *kpml4 = table_at(vmm_kernel_cr3());

    for (unsigned i4 = 0; i4 < 512; i4++) {
        uint64_t e4 = ppml4[i4];
        if (!(e4 & VMM_PRESENT) || (e4 & PG_PS) || e4 == kpml4[i4])
            continue;                          /* absent or shared kernel entry */
        uint64_t *pdpt = table_at(e4);
        for (unsigned i3 = 0; i3 < 512; i3++) {
            uint64_t e3 = pdpt[i3];
            if (!(e3 & VMM_PRESENT) || (e3 & PG_PS)) continue;
            uint64_t *pd = table_at(e3);
            for (unsigned i2 = 0; i2 < 512; i2++) {
                uint64_t e2 = pd[i2];
                if (!(e2 & VMM_PRESENT) || (e2 & PG_PS)) continue;
                uint64_t *pt = table_at(e2);
                for (unsigned i1 = 0; i1 < 512; i1++) {
                    uint64_t e1 = pt[i1];
                    if (!(e1 & VMM_PRESENT) || !(e1 & VMM_USER))
                        continue;

                    uint64_t va = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) |
                                  ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);
                    uint64_t frame = e1 & PG_ADDR;

                    if (e1 & PTE_SW_SHARED) {
                        /* vDSO etc.: share verbatim, no refcount, no COW. */
                        if (vmm_map_in(child, va, frame,
                                       (e1 & 0xFFFull) | (e1 & VMM_NX)) != 0) {
                            vmm_destroy_address_space(child);
                            return 0;
                        }
                        continue;
                    }

                    uint64_t childflags;
                    if (e1 & VMM_RW) {
                        /* Writable -> read-only + COW in both parent and child. */
                        pt[i1] = (e1 & ~VMM_RW) | PTE_SW_COW;   /* rewrite parent PTE */
                        invlpg(va);                              /* parent AS is active */
                        childflags = (e1 & 0xFFFull & ~VMM_RW) | PTE_SW_COW | (e1 & VMM_NX);
                    } else {
                        /* Read-only (e.g. text): share as-is, no COW needed. */
                        childflags = (e1 & 0xFFFull) | (e1 & VMM_NX);
                    }

                    if (vmm_map_in(child, va, frame, childflags) != 0) {
                        vmm_destroy_address_space(child);
                        return 0;
                    }
                    pmm_incref(frame);            /* parent + child now share it */
                }
            }
        }
    }
    return child;
}

int vmm_cow_fault(uint64_t cr3, uint64_t fault_va) {
    uint64_t va = fault_va & ~0xFFFull;
    uint64_t *pte = leaf_pte(cr3, va);
    if (!pte)
        return -1;
    uint64_t e = *pte;
    if (!(e & VMM_PRESENT) || !(e & PTE_SW_COW))
        return -1;                               /* not COW (e.g. real W^X fault) */

    uint64_t frame = e & PG_ADDR;
    uint64_t newflags = VMM_PRESENT | VMM_RW | (e & (VMM_USER | VMM_NX));

    if (pmm_refcount_get(frame) > 1) {
        void *copy = ptnode_alloc();             /* zeroed; fully overwritten below */
        if (!copy)
            return -1;                           /* OOM -> treat as real fault (kill) */
        memcpy(copy, (const void *)(uintptr_t)frame, (size_t)PAGE_SIZE);
        pmm_free_page(frame);                     /* drop this AS's reference */
        *pte = ((uint64_t)(uintptr_t)copy & PG_ADDR) | newflags;
    } else {
        *pte = frame | newflags;                  /* sole owner: just re-grant write */
    }
    invlpg(va);
    return 0;
}
