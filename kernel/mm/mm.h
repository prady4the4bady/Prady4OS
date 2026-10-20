/* kernel/mm/mm.h — a user address space as a refcounted object (DDR-1181 §3.1).
 * ===========================================================================
 * Before DDR-1181 the address-space state (page-table root, the anonymous-mmap
 * region table, the mmap bump pointer and the AETHER memory charge) lived in
 * struct tcb, and the page tables were destroyed when that one tcb was reaped.
 * Threads that share an address space (C1.4 P1c) need that state in ONE place
 * with a lifetime of its own, so it moves here.
 *
 * Every user tcb holds exactly one reference in t->mm; kernel threads hold
 * none (t->mm == NULL). tcb->cr3 stays as a cached copy of mm->cr3 -- the
 * context switch, tlb_shoot and the uaccess paths read it -- and the two are
 * written together (sys_exec, elf_load, the fork clone).
 *
 * The reference is dropped where the page tables used to be destroyed: the
 * wait4 reap and the orphan reaper. mm_put destroys the page tables and frees
 * the object when the last reference goes.
 * ===========================================================================
 */
#pragma once
#include <stdint.h>
#include "sched.h"          /* struct vm_area, VM_AREA_MAX */
#include "kmutex.h"         /* DDR-1181 §3.3: the mm lock */

struct mm {
    uint64_t       cr3;                 /* page-table root; 0 = none           */
    struct vm_area vma[VM_AREA_MAX];    /* anonymous mmap regions (5b)         */
    uint64_t       mmap_next;           /* bump pointer for addr==NULL mmaps   */
    uint64_t       mem_limit;           /* L6: 0 -> AETHER_MEM_DEFAULT          */
    uint64_t       mem_used;            /* charged by mmap growth (agents)     */
    uint32_t       refs;                /* tcbs holding this mm (atomic)       */
    struct kmutex  lock;                /* DDR-1181 §3.3: held by every mutation
                                         * of this address space (zeroed = free) */
};

/* Take/release the current thread's mm lock. A kernel thread has no mm and
 * nothing to lock, so both are no-ops for it. */
static inline void mm_lock(struct tcb *t)   { if (t->mm) kmutex_lock(&t->mm->lock); }
static inline void mm_unlock(struct tcb *t) { if (t->mm) kmutex_unlock(&t->mm->lock); }

/* A new mm owning `cr3` (may be 0), one reference, empty region table and the
 * mmap arena at VMM_MMAP_BASE. NULL on allocation failure; the caller still
 * owns cr3 in that case. */
struct mm *mm_create(uint64_t cr3);

/* Take / drop a reference. The last mm_put destroys mm->cr3 (if any) and
 * frees the object. mm_put(NULL) is a no-op (kernel threads). */
void mm_get(struct mm *mm);
void mm_put(struct mm *mm);
