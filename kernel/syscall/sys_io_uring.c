/* kernel/syscall/sys_io_uring.c — batched syscall ring (Phase 5b, PROC-E).
 *
 * SETUP allocates one zeroed page, maps it RW+NX into the process's mmap arena,
 * and returns its user VA — the page holds a small header plus fixed SQE/CQE
 * arrays. ENTER validates the ring VA, resolves it to the shared frame (kernel
 * identity view), runs the first `to_submit` submission entries against the named
 * fd, and posts a completion entry for each. OP_READ/OP_WRITE on FD_PIPE and
 * FD_VFS (and console writes); user buffers cross the boundary via copyin/out.
 */
#include "sys_io_uring.h"
#include "syscall.h"
#include "sched.h"
#include "string.h"
#include "mm.h"           /* DDR-1181 */
#include "fd.h"
#include "pipe.h"
#include "vfs.h"
#include "vmm.h"
#include "kheap.h"
#include "pmm.h"
#include "uaccess.h"
#include "console.h"
#include "errno.h"
#include "aether.h"     /* DDR-1170: per-task byte budget */

#define IORING_ENTRIES   8
#define IORING_OP_READ   0
#define IORING_OP_WRITE  1

struct io_uring_sqe {
    uint8_t  opcode;
    uint8_t  _pad[3];
    int32_t  fd;
    uint64_t addr;          /* user buffer VA */
    uint32_t len;
    uint32_t _pad2;
    uint64_t user_data;
} __attribute__((packed));
_Static_assert(sizeof(struct io_uring_sqe) == 32, "sqe layout");

struct io_uring_cqe {
    uint64_t user_data;
    int32_t  res;
    uint32_t _pad;
} __attribute__((packed));
_Static_assert(sizeof(struct io_uring_cqe) == 16, "cqe layout");

struct io_ring {
    uint32_t sq_tail, sq_head, cq_tail, cq_head;
    uint32_t entries, flags, _pad[2];
    struct io_uring_sqe sqes[IORING_ENTRIES];
    struct io_uring_cqe cqes[IORING_ENTRIES];
};
_Static_assert(sizeof(struct io_ring) <= 4096, "io_ring must fit one page");

static long sys_io_uring_setup_locked(long a_entries, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a_entries; (void)a2; (void)a3; (void)a4;
    struct tcb *t = current_thread;

    uint64_t va = t->mm->mmap_next;
    if (va < VMM_MMAP_BASE || va + PAGE_SIZE > VMM_MMAP_TOP)
        return -ENOMEM;
    void *frame = ptnode_alloc();                 /* zeroed shared ring page */
    if (!frame)
        return -ENOMEM;
    if (vmm_map_in(t->cr3, va, (uint64_t)(uintptr_t)frame, VMM_USER | VMM_RW | VMM_NX) != 0) {
        ptnode_free(frame);
        return -ENOMEM;
    }
    struct io_ring *r = (struct io_ring *)frame;  /* kernel identity view */
    r->entries = IORING_ENTRIES;
    t->mm->mmap_next = va + PAGE_SIZE;
    return (long)va;
}

/* Run one submission entry; returns bytes transferred or a negative errno. */
static int run_sqe(struct tcb *t, const struct io_uring_sqe *s) {
    struct fd_entry *e = fd_get(t, s->fd);
    if (!e)
        return -EBADF;
    uint32_t len = s->len;
    if (len > 256)
        len = 256;                                /* bounded per-op transfer */
    char kbuf[256];

    if (s->opcode == IORING_OP_WRITE) {
        /* DDR-1170: an SQE write emits without fd_write_user, so it charges the
         * byte budget itself; otherwise io_uring is a budget bypass. */
        if (aether_budget_charge(t, TASK_BUDGET_BYTES, len) < 0)
            sched_exit(137);                      /* never returns */
        if (copyin(kbuf, (const void __user *)(uintptr_t)s->addr, len) < 0)
            return -EFAULT;
        if (e->kind == FD_PIPE)
            return (e->flags == PIPE_WRITE_END) ? (int)pipe_write(e->pipe, kbuf, len) : -EBADF;
        if (e->kind == FD_VFS && e->file) {
            if (e->flags & FD_RB_RO)
                return -EPERM;       /* DDR-1171: rollback task, existing file */
            int n = vfs_write(e->cap, e->file, e->off, kbuf, len);
            if (n > 0) e->off += (uint64_t)n;
            return n;
        }
        if (e->kind == FD_CONSOLE) {
            for (uint32_t i = 0; i < len; i++) kputc(kbuf[i]);
            return (int)len;
        }
        return -EBADF;
    }
    if (s->opcode == IORING_OP_READ) {
        int n;
        if (e->kind == FD_PIPE) {
            if (e->flags == PIPE_WRITE_END) return -EBADF;
            n = (int)pipe_read(e->pipe, kbuf, len);
        } else if (e->kind == FD_VFS && e->file) {
            n = vfs_read(e->cap, e->file, e->off, kbuf, len);
            if (n > 0) e->off += (uint64_t)n;
        } else {
            return -EBADF;
        }
        if (n < 0)
            return n;
        if (n > 0 && copyout((void __user *)(uintptr_t)s->addr, kbuf, (size_t)n) < 0)
            return -EFAULT;
        return n;
    }
    return -EINVAL;
}

/* The ring's kernel view: the whole page validated user-RW, then its frame.
 * NULL if the page is not (or no longer) a user-RW mapping. Caller holds the
 * mm lock. */
static struct io_ring *ring_frame(struct tcb *t, uint64_t va) {
    if (!vmm_user_range_ok(t->cr3, va, PAGE_SIZE, 1))
        return 0;
    uint64_t phys = vmm_resolve(t->cr3, va);
    return phys ? (struct io_ring *)(uintptr_t)phys : 0;   /* va is aligned */
}

static long sys_io_uring_enter(long a_ring, long a_submit, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a3; (void)a4;
    struct tcb *t = current_thread;
    uint64_t va = (uint64_t)a_ring;
    int to_submit = (int)a_submit;
    if (to_submit < 0)
        return -EINVAL;
    if (to_submit > IORING_ENTRIES)
        to_submit = IORING_ENTRIES;

    /* DDR-1108: the ring VA must be PAGE-ALIGNED, checked before anything else.
     * Both checks below are applied to the page-aligned base, and the kernel
     * pointer used to be formed at phys + (va & 0xFFF) -- the CALLER's offset.
     * sizeof(struct io_ring) is 416, so any (va & 0xFFF) > 3680 put part of the
     * structure past the frame, and the loop below reads 8 SQEs and WRITES 8
     * CQEs through it: a ring-3 write into the physically adjacent frame, which
     * the caller does not own and which vmm_user_range_ok never examined. The
     * _Static_assert above bounds sizeof against a page and never could have
     * caught this -- the binding quantity is offset + sizeof.
     *
     * Page alignment rather than the weaker (va & 0xFFF) + sizeof <= PAGE_SIZE:
     * SETUP only ever returns page-aligned VAs (mmap_next starts at the aligned
     * VMM_MMAP_BASE and advances by whole pages), so nothing needs the weaker
     * form, and it would make every future field addition's safety depend on a
     * sizeof that moves whenever the structure does. */
    if (va & 0xFFFull)
        return -EINVAL;

    /* DDR-1181 §3.3: the ring is reached through its FRAME, not its VA, so a
     * sibling thread that munmapped it would leave this writing a freed page.
     * The SQEs are therefore copied out under the mm lock, run WITHOUT it (an
     * SQE can block in pipe_read or a disk read, and the lock must not be held
     * across that), and the CQEs are written back under the lock after the
     * ring is validated and resolved a second time. */
    struct io_uring_sqe sq[IORING_ENTRIES];
    mm_lock(t);
    struct io_ring *r = ring_frame(t, va);
    if (!r) {
        mm_unlock(t);
        return -EFAULT;
    }
    memcpy(sq, r->sqes, sizeof(struct io_uring_sqe) * (size_t)to_submit);
    mm_unlock(t);

    int res[IORING_ENTRIES];
    for (int i = 0; i < to_submit; i++)
        res[i] = run_sqe(t, &sq[i]);

    mm_lock(t);
    r = ring_frame(t, va);
    if (!r) {
        mm_unlock(t);
        return -EFAULT;               /* the ring was unmapped while SQEs ran */
    }
    for (int i = 0; i < to_submit; i++) {
        r->cqes[i].user_data = sq[i].user_data;
        r->cqes[i].res = res[i];
    }
    r->cq_tail = (uint32_t)to_submit;
    mm_unlock(t);
    return to_submit;
}

static long sys_io_uring_setup(long a1, long a2, long a3, long a4, long a5, long a6) {
    struct tcb *t = current_thread;
    mm_lock(t);                       /* DDR-1181 §3.3: maps a page, moves mmap_next */
    long rc = sys_io_uring_setup_locked(a1, a2, a3, a4, a5, a6);
    mm_unlock(t);
    return rc;
}

void sys_io_uring_register(void) {
    syscall_register(SYS_IO_URING_SETUP, sys_io_uring_setup);
    syscall_register(SYS_IO_URING_ENTER, sys_io_uring_enter);
}
