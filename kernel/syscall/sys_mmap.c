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
 * DDR-1178 P1: MAP_SHARED|MAP_ANONYMOUS is accepted (shared across fork via
 * PTE_SW_SHMEM). File-backed MAP_SHARED stays refused: it needs write-back and
 * a shared page cache (DDR-1178 P4, owner decisions D4/D5).
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
#include "mm.h"           /* DDR-1181 */
#include "vmm.h"
#include "kheap.h"     /* ptnode_alloc / ptnode_free */
#include "pmm.h"       /* PAGE_SIZE */
#include "errno.h"
#include "aether.h"    /* per-agent memory cap (Layer 6, ADR-026) */
#include "fd.h"        /* DDR-1112: fd_get / struct fd_entry (file-backed) */
#include "vfs.h"       /* DDR-1112: vfs_read — pread-style, takes an offset */
#include "shm.h"       /* DDR-1178 A2: SysV segment objects */
#include "cap.h"

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_ANONYMOUS  0x20

static struct vm_area *vma_find(struct tcb *t, uint64_t base) {
    for (int i = 0; i < VM_AREA_MAX; i++)
        if (t->mm->vma[i].npages && t->mm->vma[i].base == base)
            return &t->mm->vma[i];
    return 0;
}

static struct vm_area *vma_free_slot(struct tcb *t) {
    for (int i = 0; i < VM_AREA_MAX; i++)
        if (t->mm->vma[i].npages == 0)
            return &t->mm->vma[i];
    return 0;
}

static int vma_overlaps(struct tcb *t, uint64_t base, uint64_t npages) {
    uint64_t end = base + npages * PAGE_SIZE;
    for (int i = 0; i < VM_AREA_MAX; i++) {
        if (!t->mm->vma[i].npages)
            continue;
        uint64_t b = t->mm->vma[i].base;
        uint64_t e = b + t->mm->vma[i].npages * PAGE_SIZE;
        if (base < e && b < end)
            return 1;
    }
    return 0;
}

static void unmap_free_frame(uint64_t ph) {
    ptnode_free((void *)(uintptr_t)ph);
}

/* Unmap + free `npages` starting at `base` in `cr3`'s address space (active AS
 * == cr3 during a syscall). DDR-1177 §3.4: the frames are freed only AFTER the
 * cross-CPU shootdown has been acked -- freeing first was the use-after-free
 * of DDR-1177 §1 once two CPUs can run one AS. */
static void unmap_range(uint64_t cr3, uint64_t base, uint64_t npages) {
    vmm_unmap_range_flush(cr3, base, npages, unmap_free_frame);
}

/* The mapping's base: the arena bump pointer for addr 0, else the page-aligned
 * hint; inside the arena and overlapping no region (no silent replace). */
static int mmap_pick_base(struct tcb *t, uint64_t addr, uint64_t npages, uint64_t *out) {
    uint64_t base;
    if (addr == 0) {
        base = t->mm->mmap_next;                     /* kernel-chosen (bump) */
    } else {
        if (addr & (PAGE_SIZE - 1))
            return -EINVAL;                      /* hint must be page-aligned */
        base = addr;
    }
    if (base < VMM_MMAP_BASE || base + npages * PAGE_SIZE > VMM_MMAP_TOP)
        return -EINVAL;                          /* outside the mmap arena */
    if (vma_overlaps(t, base, npages))
        return -EINVAL;                          /* no silent replace (baseline) */
    *out = base;
    return 0;
}

/* Map pages [first, first + npages) of shm object `obj` at `base`, one
 * pmm_incref per frame (this mapping's reference). 0, or -ENOMEM with the
 * partial range unmapped; the caller owns the attach either way. */
static int map_obj_pages(struct tcb *t, int obj, uint32_t first, uint32_t npages,
                         uint64_t base, uint64_t pflags) {
    for (uint32_t i = 0; i < npages; i++) {
        uint64_t f = shm_obj_frame(obj, first + i);
        pmm_incref(f);
        if (vmm_map_in(t->cr3, base + (uint64_t)i * PAGE_SIZE, f, pflags) != 0) {
            ptnode_free((void *)(uintptr_t)f);
            unmap_range(t->cr3, base, i);
            return -ENOMEM;
        }
    }
    return 0;
}

/* DDR-1178 A3.2: mmap(MAP_SHARED) of an FD_SHM fd is an attach of pages
 * off/4096 .. of the object, with the mmap address rules. */
static long shm_fd_map(struct tcb *t, int obj, uint64_t addr, uint64_t len, int prot,
                       long a_off) {
    if (a_off < 0 || ((uint64_t)a_off & (PAGE_SIZE - 1)))
        return -EINVAL;
    uint64_t npages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t first = (uint64_t)a_off / PAGE_SIZE;
    uint64_t base;
    int rc = mmap_pick_base(t, addr, npages, &base);
    if (rc < 0)
        return rc;
    if (shm_attach_begin(obj) < 0)
        return -EINVAL;                          /* not sized yet */
    if (first + npages > shm_obj_npages(obj)) {
        shm_detach(obj);
        return -EINVAL;                          /* past the object's size */
    }
    struct vm_area *v = vma_free_slot(t);
    if (!v) {
        shm_detach(obj);
        return -ENOMEM;
    }
    uint64_t pflags = VMM_USER | VMM_NX | PTE_SW_SHMEM | ((prot & PROT_WRITE) ? VMM_RW : 0);
    if (map_obj_pages(t, obj, (uint32_t)first, (uint32_t)npages, base, pflags) != 0) {
        shm_detach(obj);
        return -ENOMEM;
    }
    v->base = base;
    v->npages = npages;
    v->shm = obj + 1;
    v->shm_ro = 0;                               /* the fd is O_RDWR (A3.1) */
    if (addr == 0)
        t->mm->mmap_next = base + npages * PAGE_SIZE;
    return (long)base;
}

static long sys_mmap_locked(long a_addr, long a_len, long a_prot, long a_flags,
                     long a_fd, long a_off) {
    struct tcb *t = current_thread;
    uint64_t addr = (uint64_t)a_addr;
    uint64_t len  = (uint64_t)a_len;
    int prot  = (int)a_prot;
    int flags = (int)a_flags;

    if (len == 0)
        return -EINVAL;
    /* DDR-1178 P1: MAP_SHARED is anonymous-only. File-backed shared mappings
     * need write-back and a shared page cache (P4, owner decisions D4/D5). */
    int shared = (flags & MAP_SHARED) != 0;
    if (shared && (flags & MAP_PRIVATE))
        return -EINVAL;                         /* POSIX: exactly one of the two */
    if (prot & PROT_EXEC)
        return -EINVAL;                         /* W^X: no executable mapping */
    if (shared && a_fd != -1) {
        /* DDR-1178 A3: a shm_open object. Any other fd stays refused: shared
         * file mappings need write-back (P4). */
        struct fd_entry *se = fd_get(t, (int)a_fd);
        if (!se || se->kind != FD_SHM || (flags & MAP_ANONYMOUS))
            return -EINVAL;
        if (t->is_agent)
            return -EPERM;                      /* A2.2, as shmat */
        return shm_fd_map(t, se->mnt, addr, len, prot, a_off);
    }

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
        if (fe->kind == FD_SHM)
            return -EINVAL;                     /* DDR-1178 A3.2: no private copy */
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
    int brc = mmap_pick_base(t, addr, npages, &base);
    if (brc < 0)
        return brc;

    struct vm_area *v = vma_free_slot(t);
    if (!v)
        return -ENOMEM;                          /* too many regions */

    /* AETHER memory cap (ADR-026 D5): charge this growth against the agent's
     * 128 MiB hard cap; an over-cap agent is cleanly killed, never a panic. */
    if (aether_mem_charge(t, npages * PAGE_SIZE) < 0) {
        mm_unlock(t);                            /* DDR-1181: never exit holding it */
        sched_exit(137);                         /* AGENT_OOM_KILLED; never returns */
    }

    uint64_t pflags = VMM_USER | VMM_NX;
    if (prot & PROT_WRITE)
        pflags |= VMM_RW;
    if (shared)
        pflags |= PTE_SW_SHMEM;                  /* DDR-1178: fork shares, not COWs */

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
        t->mm->mmap_next = base + npages * PAGE_SIZE;
    return (long)base;
}

static long sys_munmap_locked(long a_addr, long a_len, long a3, long a4, long a5, long a6) {
    (void)a_len; (void)a3; (void)a4; (void)a5; (void)a6;  /* whole-region unmap */
    struct tcb *t = current_thread;
    struct vm_area *v = vma_find(t, (uint64_t)a_addr);
    if (!v)
        return -EINVAL;
    unmap_range(t->cr3, v->base, v->npages);
    if (v->shm) {
        shm_detach(v->shm - 1);      /* DDR-1178 A2.2: munmap is a detach */
        v->shm = 0;
        v->shm_ro = 0;
    } else {
        aether_mem_uncharge(t, v->npages * PAGE_SIZE);
    }
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
static long sys_mprotect_locked(long a1, long a2, long a3, long a4, long a5, long a6) {
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

    /* DDR-1178 A2.2: write cannot be added back to a SHM_RDONLY attachment. */
    if (prot & PROT_WRITE) {
        uint64_t end = addr + ((len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1));
        for (int i = 0; i < VM_AREA_MAX; i++) {
            struct vm_area *v = &t->mm->vma[i];
            if (v->npages && v->shm_ro &&
                addr < v->base + v->npages * PAGE_SIZE && v->base < end)
                return -EACCES;
        }
    }

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

/* ---- DDR-1178 Amendment 2: SysV shared memory (IPC_PRIVATE only) ----------
 * A shmid is a RES_SHM capability in the caller's own table, encoded as
 * (generation << 6) | slot (A2.1): it reaches the segment only from a table
 * that holds it -- the creator's, its threads', and fork children's. */
#define IPC_PRIVATE 0
#define IPC_RMID    0
#define SHM_RDONLY  010000            /* Linux value */
#define SHMID_GEN_MAX (1u << 24)

static cap_t shmid_handle(long id) {
    return ((cap_t)((uint64_t)id >> 6) << 32) | (cap_t)(id & 63);
}

/* The segment object a shmid names in `t`'s table, or -EINVAL. */
static int shmid_obj(struct tcb *t, long id) {
    if (id < 0 || id > 0x7FFFFFFFL)
        return -EINVAL;
    uint64_t res = 0;
    uint32_t rights = 0;
    if (!cap_lookup(t->caps, shmid_handle(id), RES_SHM, &res, &rights))
        return -EINVAL;
    return (int)res;
}

static long sys_shmget_impl(long a_key, long a_size, long a_flags, long a4, long a5, long a6) {
    (void)a_flags; (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_thread;
    if (t->is_agent)
        return -EPERM;               /* A2.2: a segment is memory the agent cap does not see */
    if (a_key != IPC_PRIVATE)
        return -EACCES;              /* D1(c): no global namespace */
    if (a_size <= 0 || (uint64_t)a_size > (uint64_t)SHM_MAX_PAGES * PAGE_SIZE)
        return -EINVAL;
    int obj = shm_obj_create((uint32_t)(((uint64_t)a_size + PAGE_SIZE - 1) / PAGE_SIZE));
    if (obj < 0)
        return obj;
    cap_t h = cap_create(t->caps, RES_SHM, (uint64_t)obj, CAP_SHM_R | CAP_SHM_W);
    /* The fresh object has holders == 0 until this capability holds it; if no
     * capability could be minted, one hold/unhold pair reclaims it. */
    if (h == CAP_NULL) {
        shm_hold(obj);
        shm_unhold(obj);
        return -ENOSPC;
    }
    uint64_t gen = (uint64_t)h >> 32;
    if (gen >= SHMID_GEN_MAX) {
        cap_revoke(t->caps, h);      /* unholds: the object is reclaimed */
        return -ENOSPC;
    }
    return (long)((gen << 6) | ((uint64_t)h & 63));
}

static long sys_shmat_locked(long a_id, long a_addr, long a_flags, long a4, long a5, long a6) {
    (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_thread;
    if (t->is_agent)
        return -EPERM;
    if (a_addr != 0)
        return -EINVAL;              /* P2: kernel-chosen address only */
    int obj = shmid_obj(t, a_id);
    if (obj < 0)
        return obj;
    int rc = shm_attach_begin(obj);
    if (rc < 0)
        return rc;                   /* -EIDRM when marked (D2) */
    uint32_t npages = shm_obj_npages(obj);
    uint64_t base = t->mm->mmap_next;
    struct vm_area *v = vma_free_slot(t);
    if (!v || base + (uint64_t)npages * PAGE_SIZE > VMM_MMAP_TOP ||
        vma_overlaps(t, base, npages)) {
        shm_detach(obj);
        return -ENOMEM;
    }
    int ro = (a_flags & SHM_RDONLY) != 0;
    uint64_t pflags = VMM_USER | VMM_NX | PTE_SW_SHMEM | (ro ? 0 : VMM_RW);
    if (map_obj_pages(t, obj, 0, npages, base, pflags) != 0) {
        shm_detach(obj);
        return -ENOMEM;
    }
    v->base = base;
    v->npages = npages;
    v->shm = obj + 1;
    v->shm_ro = ro;
    t->mm->mmap_next = base + (uint64_t)npages * PAGE_SIZE;
    return (long)base;
}

static long sys_shmdt_locked(long a_addr, long a2, long a3, long a4, long a5, long a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct vm_area *v = vma_find(current_thread, (uint64_t)a_addr);
    if (!v || !v->shm)
        return -EINVAL;
    return sys_munmap_locked(a_addr, 0, 0, 0, 0, 0);
}

static long sys_shmctl_impl(long a_id, long a_cmd, long a_buf, long a4, long a5, long a6) {
    (void)a_buf; (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_thread;
    if (t->is_agent)
        return -EPERM;
    if (a_cmd != IPC_RMID)
        return -EINVAL;              /* IPC_STAT / IPC_SET: not in P2 */
    int obj = shmid_obj(t, a_id);
    if (obj < 0)
        return obj;
    return shm_mark(obj);
}

/* ---- DDR-1178 Amendment 3: POSIX shm_open(SHM_ANON) ----------------------
 * D1(c): no names. The fd is the only way to reach the object, and it reaches
 * other processes only through fork (fd_clone takes a hold). */
#define SHM_ANON      1L             /* FreeBSD's (const char *)1 */
#define SHMO_ACCMODE  3
#define SHMO_RDWR     2
#define SHMO_CREAT    0x40
#define SHMO_EXCL     0x80

static long sys_shm_open_impl(long a_name, long a_oflag, long a_mode, long a4, long a5, long a6) {
    (void)a_mode; (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_thread;
    if (t->is_agent)
        return -EPERM;               /* A2.2: memory the agent cap does not see */
    if (a_name != SHM_ANON)
        return -EACCES;              /* D1(c); the pointer is never read */
    if ((a_oflag & SHMO_ACCMODE) != SHMO_RDWR ||
        (a_oflag & ~(long)(SHMO_ACCMODE | SHMO_CREAT | SHMO_EXCL)))
        return -EINVAL;
    int fd = fd_alloc(t);
    if (fd < 0)
        return -EMFILE;
    int obj = shm_obj_reserve();     /* held once: by this entry */
    if (obj < 0)
        return obj;
    struct fd_entry *e = &t->fdt.e[fd];
    e->kind  = FD_SHM;
    e->mnt   = obj;
    e->off   = 0;
    e->file  = 0;
    e->pipe  = 0;
    e->epoll = 0;
    e->cap   = 0;
    e->flags = SHMO_RDWR;
    return fd;
}

/* DDR-1181 §3.3: every change to the address space holds the mm lock. The
 * bodies above are unchanged; they run with it held. */
static long sys_mmap(long a1, long a2, long a3, long a4, long a5, long a6) {
    struct tcb *t = current_thread;
    mm_lock(t);
    long rc = sys_mmap_locked(a1, a2, a3, a4, a5, a6);
    mm_unlock(t);
    return rc;
}

static long sys_munmap(long a1, long a2, long a3, long a4, long a5, long a6) {
    struct tcb *t = current_thread;
    mm_lock(t);
    long rc = sys_munmap_locked(a1, a2, a3, a4, a5, a6);
    mm_unlock(t);
    return rc;
}

static long sys_mprotect(long a1, long a2, long a3, long a4, long a5, long a6) {
    struct tcb *t = current_thread;
    mm_lock(t);
    long rc = sys_mprotect_locked(a1, a2, a3, a4, a5, a6);
    mm_unlock(t);
    return rc;
}

static long sys_shmat(long a1, long a2, long a3, long a4, long a5, long a6) {
    struct tcb *t = current_thread;
    mm_lock(t);
    long rc = sys_shmat_locked(a1, a2, a3, a4, a5, a6);
    mm_unlock(t);
    return rc;
}

static long sys_shmdt(long a1, long a2, long a3, long a4, long a5, long a6) {
    struct tcb *t = current_thread;
    mm_lock(t);
    long rc = sys_shmdt_locked(a1, a2, a3, a4, a5, a6);
    mm_unlock(t);
    return rc;
}

void sys_mmap_register(void) {
    syscall_register(SYS_MMAP,     sys_mmap);
    syscall_register(SYS_MUNMAP,   sys_munmap);
    syscall_register(SYS_MPROTECT, sys_mprotect);   /* DDR-1031 */
    syscall_register(SYS_SHMGET,   sys_shmget_impl);    /* DDR-1178 A2 */
    syscall_register(SYS_SHMAT,    sys_shmat);
    syscall_register(SYS_SHMDT,    sys_shmdt);
    syscall_register(SYS_SHMCTL,   sys_shmctl_impl);
    syscall_register(SYS_SHM_OPEN, sys_shm_open_impl);  /* DDR-1178 A3 */
}
