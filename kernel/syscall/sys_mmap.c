/* kernel/syscall/sys_mmap.c — anonymous mmap baseline (Phase 5b slice 6, ADR-022).
 *
 * Scope: MAP_PRIVATE only, RW+NX pages in the user mmap arena, either
 * MAP_ANONYMOUS or file-backed. PROT_EXEC is rejected (W^X — executable code
 * arrives via the ELF loader, not mmap). MAP_SHARED, MAP_FIXED replace
 * semantics, partial munmap, demand paging, msync and mremap are deferred
 * (see ADR-022 / docs/build_status.md).
 *
 * DDR-1112: file-backed MAP_PRIVATE is implemented and is EAGER — the frame is
 * filled from the file BEFORE it is mapped, so no page-fault path is involved.
 * Four facts make that small, each measured rather than assumed:
 *   (a) vfs_read is already pread-style (vfs.h: explicit `off`, and struct
 *       vfs_file carries NO cursor — the seek position is fd_entry.off, one
 *       layer up), so filling a page DOES NOT MOVE THE CALLER'S FILE POSITION,
 *       which is a side effect POSIX mmap must not have. Do not "simplify" the
 *       read below to use e->off; smoke-sysmmap's cursor arm exists to catch it.
 *   (b) ptnode_alloc returns a kernel-usable pointer into the identity-mapped
 *       low 1 GiB, so the kernel writes the frame directly — copyout,
 *       vmm_user_range_ok and SMAP are not on this path at all.
 *   (c) the capability comes from the fd (e->cap), so a mapping inherits
 *       exactly the read right the open established; nothing is minted here.
 *   (d) NO struct vm_area change is needed: MAP_PRIVATE has no write-back, so
 *       the pages are ptnode_alloc'd and freed identically to anonymous ones
 *       and munmap need not know where the bytes came from. Dirty tracking and
 *       msync are MAP_SHARED's problem, and msync has no subject under PRIVATE.
 * MAP_SHARED stays refused: it needs write-back, a shared page cache, and a
 * cross-CPU TLB shootdown this kernel does not have (DDR-1075 sec.3 / DDR-1077).
 *
 * DDR-877 (item 19): this is now the real POSIX six-argument mmap. The 4-arg
 * form was worse than incomplete — a caller passing fd and offset had them
 * silently discarded and got anonymous zero pages back, i.e. "map this file"
 * succeeded and returned something else entirely. THAT IS ALSO WHY THE GATE
 * ASSERTS THE FILE'S OWN BYTES rather than merely that the call succeeded: a
 * build that accepted the fd and handed back zero pages would pass the weaker
 * arm and reintroduce exactly this defect.
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
#include "fd.h"        /* DDR-1112: fd_get / struct fd_entry (file-backed) */
#include "vfs.h"       /* DDR-1112: vfs_read — pread-style, takes an offset */

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
    if (flags & MAP_SHARED)
        return -EINVAL;                         /* private only (see header) */
    if (prot & PROT_EXEC)
        return -EINVAL;                         /* W^X: no executable mapping */

    /* fd == -1 is POSIX's anonymous form and takes the original path VERBATIM,
     * so every existing caller and every green smoke-sysmmap arm is untouched
     * by construction (the DDR-1032 shape). Note that some libcs pass fd == 0
     * for an anonymous map, which is a real open descriptor (stdin); that is
     * NOT absorbed — it is now a genuine request to map stdin and is answered
     * on its merits below, which for a console fd is -ENODEV. */
    struct fd_entry *fe = 0;
    if (a_fd == -1) {
        if (!(flags & MAP_ANONYMOUS))
            return -EINVAL;                     /* no fd and not anonymous */
        if (a_off != 0)
            return -EINVAL;                     /* offset meaningless for anon */
    } else {
        /* DDR-1112: file-backed MAP_PRIVATE. */
        if (flags & MAP_ANONYMOUS)
            return -EINVAL;                     /* an fd AND "anonymous" is a
                                                 * contradiction; absorbing it
                                                 * silently is DDR-877's defect */
        fe = fd_get(t, (int)a_fd);
        if (!fe || fe->kind == FD_NONE)
            return -EBADF;
        if (fe->kind != FD_VFS || !fe->file)
            return -ENODEV;                     /* a pipe or the console has no
                                                 * byte at an offset. Distinct
                                                 * from -EBADF on purpose, per
                                                 * DDR-1080: the return value
                                                 * should name its own family */
        if (a_off < 0 || ((uint64_t)a_off & (PAGE_SIZE - 1)))
            return -EINVAL;                     /* POSIX requires a page-aligned
                                                 * offset — and after this the
                                                 * arithmetic below is PROVABLY
                                                 * aligned, so the invariant is
                                                 * visible in the code rather
                                                 * than argued in a comment */
    }

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
        /* DDR-1112: fill the frame from the file BEFORE mapping it. The frame is
         * kernel-writable here (header (b)), so this is a plain kernel memcpy
         * target and no user-pointer machinery is involved.
         *
         * THE EOF TAIL IS FREE AND IS EXACTLY POSIX: ptnode_alloc zeroed the
         * page, and a short read at or past end-of-file simply leaves the
         * remainder zero — which is what POSIX says the bytes beyond the file's
         * end must read as. So the correct behaviour falls out of doing nothing,
         * and only a NEGATIVE return needs a branch. */
        if (fe) {
            int n = vfs_read(fe->cap, fe->file,
                             (uint64_t)a_off + i * PAGE_SIZE,
                             frame, (uint32_t)PAGE_SIZE);
            if (n < 0) {
                ptnode_free(frame);
                unmap_range(t->cr3, base, i);
                return -EIO;
            }
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

/* DDR-1031: SYS_MPROTECT (NSI 97) -- change an existing user mapping's
 * permissions, keeping its frames. The range walk lives in vmm_protect_range;
 * the policy lives here.
 *
 * Three refusals, each with a reason (DDR-1031 §3):
 *   PROT_WRITE|PROT_EXEC  -- W^X is this kernel's posture (DDR-757); a syscall
 *                            that handed ring 3 a W+X page would be a hole
 *                            straight through it.
 *   PROT_WRITE on a COW page -- the hardware RO bit IS the copy trigger, so
 *                            granting write would let this process write a frame
 *                            another still shares, with no copy and no fault.
 *                            Detected in vmm_protect_range, reported as -EACCES.
 *   PROT_NONE             -- making a user page absent collides with the
 *                            demand-paged stack (ADR-038), which faults absent
 *                            user pages IN rather than reporting them. Telling
 *                            the two apart needs a state that does not exist.
 */
static long sys_mprotect(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a4; (void)a5; (void)a6;
    uint64_t addr = (uint64_t)a1;
    uint64_t len  = (uint64_t)a2;
    int prot      = (int)a3;

    if (addr & 0xFFFull)                       /* POSIX: addr must be page-aligned */
        return -EINVAL;
    if (len == 0)
        return 0;
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;
    if (prot == 0)                             /* PROT_NONE -- see the note above */
        return -EINVAL;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC))
        return -EACCES;                        /* W^X */

    /* Overflow-safe bound, then the same user-VA window mmap uses. */
    if (addr < VMM_USER_MIN || len > (VMM_USER_MAX - addr))
        return -EINVAL;

    struct tcb *t = current_thread;
    if (!t)
        return -ESRCH;

    uint64_t flags = 0;
    if (prot & PROT_WRITE) flags |= VMM_RW;
    if (!(prot & PROT_EXEC)) flags |= VMM_NX;   /* readable+non-exec is the default */

    int rc = vmm_protect_range(t->cr3, addr, len, flags);
    if (rc == -2)
        return -EACCES;                        /* write asked on a COW page */
    if (rc != 0)
        return -ENOMEM;                        /* a page in the range is absent */
    return 0;
}

void sys_mmap_register(void) {
    syscall_register(SYS_MMAP,     sys_mmap);
    syscall_register(SYS_MUNMAP,   sys_munmap);
    syscall_register(SYS_MPROTECT, sys_mprotect);   /* DDR-1031 */
}
