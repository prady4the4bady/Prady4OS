/* kernel/syscall/sys_mmap.c — anonymous mmap baseline (Phase 5b slice 6, ADR-022).
 *
 * Scope (baseline): MAP_ANONYMOUS | MAP_PRIVATE only, RW+NX pages in the user
 * mmap arena. PROT_EXEC is rejected (W^X — anon regions are data; executable
 * code arrives via the ELF loader, not mmap). File-backed (MAP_SHARED / fd),
 * MAP_FIXED replace semantics, partial munmap, demand paging and mremap are
 * deferred (see ADR-022 / docs/build_status.md).
 *
 * DDR-877 (item 19): this is now the real POSIX six-argument mmap. The 4-arg
 * form was worse than incomplete — a caller passing fd and offset had them
 * silently discarded and got anonymous zero pages back, i.e. "map this file"
 * succeeded and returned something else entirely. fd and offset are now read
 * and REJECTED when they ask for something this implementation does not do.
 *
 * Pages are ptnode_alloc'd (like the ELF loader) so vmm_destroy_address_space
 * reclaims any still-mapped region when the process is reaped.
 */
#include "sys_mmap.h"
#include "syscall.h"
#include "sched.h"
#include "vmm.h"
#include "kheap.h"     /* ptnode_alloc / ptnode_free */
#include "pmm.h"       /* PAGE_SIZE */
#include "errno.h"
#include "aether.h"    /* per-agent memory cap (Layer 6, ADR-026) */

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_ANONYMOUS  0x20

static struct vm_area *vma_find(struct tcb *t, uint64_t base) {
    for (int i = 0; i < VM_AREA_MAX; i++)
        if (t->vma[i].npages && t->vma[i].base == base)
            return &t->vma[i];
    return 0;
}

static struct vm_area *vma_free_slot(struct tcb *t) {
    for (int i = 0; i < VM_AREA_MAX; i++)
        if (t->vma[i].npages == 0)
            return &t->vma[i];
    return 0;
}

static int vma_overlaps(struct tcb *t, uint64_t base, uint64_t npages) {
    uint64_t end = base + npages * PAGE_SIZE;
    for (int i = 0; i < VM_AREA_MAX; i++) {
        if (!t->vma[i].npages)
            continue;
        uint64_t b = t->vma[i].base;
        uint64_t e = b + t->vma[i].npages * PAGE_SIZE;
        if (base < e && b < end)
            return 1;
    }
    return 0;
}

/* Unmap + free `npages` starting at `base` in `cr3`'s address space. */
static void unmap_range(uint64_t cr3, uint64_t base, uint64_t npages) {
    for (uint64_t i = 0; i < npages; i++) {
        uint64_t va = base + i * PAGE_SIZE;
        uint64_t ph = vmm_resolve(cr3, va);
        vmm_unmap(va);                          /* active AS == cr3 during a syscall */
        if (ph)
            ptnode_free((void *)(uintptr_t)ph);
    }
}

static long sys_mmap(long a_addr, long a_len, long a_prot, long a_flags,
                     long a_fd, long a_off) {
    struct tcb *t = current_thread;
    uint64_t addr = (uint64_t)a_addr;
    uint64_t len  = (uint64_t)a_len;
    int prot  = (int)a_prot;
    int flags = (int)a_flags;

    if (len == 0)
        return -EINVAL;
    if (!(flags & MAP_ANONYMOUS) || (flags & MAP_SHARED))
        return -EINVAL;                         /* anonymous private only */
    /* POSIX: an anonymous mapping carries fd == -1 and offset == 0. Some libcs
     * pass fd == 0 instead, which is a real, open file descriptor — accepting
     * it would mean silently ignoring a request to map stdin. Both are refused
     * rather than absorbed: a file-backed mapping is not implemented, so the
     * only honest answer is an error, not zero pages that look like success. */
    if (a_fd != -1)
        return -ENOSYS;                         /* file-backed mmap: not built */
    if (a_off != 0)
        return -EINVAL;                         /* offset is meaningless for anon */
    if (prot & PROT_EXEC)
        return -EINVAL;                         /* W^X: no executable anon page */

    uint64_t npages = (len + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t base;
    if (addr == 0) {
        base = t->mmap_next;                     /* kernel-chosen (bump) */
    } else {
        if (addr & (PAGE_SIZE - 1))
            return -EINVAL;                      /* hint must be page-aligned */
        base = addr;
    }
    if (base < VMM_MMAP_BASE || base + npages * PAGE_SIZE > VMM_MMAP_TOP)
        return -EINVAL;                          /* outside the mmap arena */
    if (vma_overlaps(t, base, npages))
        return -EINVAL;                          /* no silent replace (baseline) */

    struct vm_area *v = vma_free_slot(t);
    if (!v)
        return -ENOMEM;                          /* too many regions */

    /* AETHER memory cap (ADR-026 D5): charge this growth against the agent's
     * 128 MiB hard cap; an over-cap agent is cleanly killed, never a panic. */
    if (aether_mem_charge(t, npages * PAGE_SIZE) < 0)
        sched_exit(137);                         /* AGENT_OOM_KILLED; never returns */

    uint64_t pflags = VMM_USER | VMM_NX;
    if (prot & PROT_WRITE)
        pflags |= VMM_RW;

    for (uint64_t i = 0; i < npages; i++) {
        void *frame = ptnode_alloc();            /* zeroed */
        if (!frame) {
            unmap_range(t->cr3, base, i);
            return -ENOMEM;
        }
        if (vmm_map_in(t->cr3, base + i * PAGE_SIZE, (uint64_t)(uintptr_t)frame, pflags) != 0) {
            ptnode_free(frame);
            unmap_range(t->cr3, base, i);
            return -ENOMEM;
        }
    }

    v->base   = base;
    v->npages = npages;
    if (addr == 0)
        t->mmap_next = base + npages * PAGE_SIZE;
    return (long)base;
}

static long sys_munmap(long a_addr, long a_len, long a3, long a4, long a5, long a6) {
    (void)a_len; (void)a3; (void)a4; (void)a5; (void)a6;  /* whole-region unmap */
    struct tcb *t = current_thread;
    struct vm_area *v = vma_find(t, (uint64_t)a_addr);
    if (!v)
        return -EINVAL;
    unmap_range(t->cr3, v->base, v->npages);
    aether_mem_uncharge(t, v->npages * PAGE_SIZE);
    v->base = 0;
    v->npages = 0;
    return 0;
}

void sys_mmap_register(void) {
    syscall_register(SYS_MMAP,   sys_mmap);
    syscall_register(SYS_MUNMAP, sys_munmap);
}
