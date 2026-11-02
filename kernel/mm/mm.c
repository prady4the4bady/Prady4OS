/* kernel/mm/mm.c — refcounted user address space (DDR-1181 §3.1). */
#include "mm.h"
#include "vmm.h"
#include "kheap.h"
#include "string.h"

struct mm *mm_create(uint64_t cr3) {
    struct mm *mm = (struct mm *)kmalloc(sizeof *mm);
    if (!mm)
        return 0;
    memset(mm, 0, sizeof *mm);      /* kmalloc does not zero (CLAUDE.md rule 11) */
    mm->cr3       = cr3;
    mm->mmap_next = VMM_MMAP_BASE;
    mm->refs      = 1;
    mm->live      = 1;
    return mm;
}

void mm_get(struct mm *mm) {
    __atomic_add_fetch(&mm->refs, 1, __ATOMIC_ACQ_REL);
}

void mm_put(struct mm *mm) {
    if (!mm)
        return;
    if (__atomic_sub_fetch(&mm->refs, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    if (mm->cr3)
        vmm_destroy_address_space(mm->cr3);
    kfree(mm);
}
