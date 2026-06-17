/* kernel/vmm.c — kernel-owned 4-level paging (Phase 2b).
 *
 * Tables are reached via the low identity map (physical addr == virtual addr for
 * the low 1 GiB), so a table's physical address doubles as a usable pointer.
 * Intermediate tables are allocated as zeroed pages via ptnode_alloc and freed
 * via ptnode_free, keeping the heap's leak accounting consistent.
 */
#include "vmm.h"
#include "pmm.h"
#include "kheap.h"

#define PTE_PRESENT  0x1ull
#define PTE_PS       0x80ull
#define PTE_ADDR     0x000FFFFFFFFFF000ull

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void invlpg(uint64_t v) {
    __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
}

/* A table's physical address is directly usable through the identity map. */
static uint64_t *table_at(uint64_t phys) {
    return (uint64_t *)(uintptr_t)phys;
}

static unsigned idx(uint64_t virt, unsigned level) {  /* level: 4=PML4 .. 1=PT */
    return (unsigned)((virt >> (12 + 9 * (level - 1))) & 0x1FF);
}

/* Return the next-level table's physical address, creating it if `create`. */
static uint64_t descend(uint64_t *table, unsigned i, int create) {
    uint64_t e = table[i];
    if (e & PTE_PRESENT) {
        if (e & PTE_PS)
            return 0;                 /* huge page here; cannot descend */
        return e & PTE_ADDR;
    }
    if (!create)
        return 0;
    void *frame = ptnode_alloc();     /* zeroed page from the PMM */
    if (!frame)
        return 0;
    uint64_t fphys = (uint64_t)(uintptr_t)frame;
    table[i] = fphys | PTE_PRESENT | VMM_RW;
    return fphys;
}

static int table_empty(const uint64_t *t) {
    for (unsigned i = 0; i < 512; i++) {
        if (t[i] != 0)
            return 0;
    }
    return 1;
}

int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pml4 = table_at(read_cr3() & PTE_ADDR);

    uint64_t pdpt = descend(pml4, idx(virt, 4), 1);
    if (!pdpt) return -1;
    uint64_t pd = descend(table_at(pdpt), idx(virt, 3), 1);
    if (!pd) return -1;
    uint64_t pt = descend(table_at(pd), idx(virt, 2), 1);
    if (!pt) return -1;

    table_at(pt)[idx(virt, 1)] = (phys & PTE_ADDR) | (flags & 0xFFF) | PTE_PRESENT;
    invlpg(virt);
    return 0;
}

int vmm_unmap(uint64_t virt) {
    uint64_t *pml4 = table_at(read_cr3() & PTE_ADDR);

    uint64_t pdpt_p = descend(pml4, idx(virt, 4), 0);
    if (!pdpt_p) return -1;
    uint64_t *pdpt = table_at(pdpt_p);
    uint64_t pd_p = descend(pdpt, idx(virt, 3), 0);
    if (!pd_p) return -1;
    uint64_t *pd = table_at(pd_p);
    uint64_t pt_p = descend(pd, idx(virt, 2), 0);
    if (!pt_p) return -1;
    uint64_t *pt = table_at(pt_p);

    pt[idx(virt, 1)] = 0;
    invlpg(virt);

    /* Reclaim now-empty tables bottom-up: each parent's entry is at the index
     * for the child's level (PD entry -> level 2, PDPT entry -> 3, PML4 -> 4). */
    if (table_empty(pt)) {
        ptnode_free(pt);
        pd[idx(virt, 2)] = 0;
        if (table_empty(pd)) {
            ptnode_free(pd);
            pdpt[idx(virt, 3)] = 0;
            if (table_empty(pdpt)) {
                ptnode_free(pdpt);
                pml4[idx(virt, 4)] = 0;
            }
        }
    }
    return 0;
}
