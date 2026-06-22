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
#include "fd.h"
#include "pipe.h"
#include "vfs.h"
#include "vmm.h"
#include "kheap.h"
#include "pmm.h"
#include "uaccess.h"
#include "console.h"
#include "errno.h"

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

static long sys_io_uring_setup(long a_entries, long a2, long a3, long a4) {
    (void)a_entries; (void)a2; (void)a3; (void)a4;
    struct tcb *t = current_thread;

    uint64_t va = t->mmap_next;
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
    t->mmap_next = va + PAGE_SIZE;
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
        if (copyin(kbuf, (const void __user *)(uintptr_t)s->addr, len) < 0)
            return -EFAULT;
        if (e->kind == FD_PIPE)
            return (e->flags == PIPE_WRITE_END) ? (int)pipe_write(e->pipe, kbuf, len) : -EBADF;
        if (e->kind == FD_VFS && e->file) {
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

static long sys_io_uring_enter(long a_ring, long a_submit, long a3, long a4) {
    (void)a3; (void)a4;
    struct tcb *t = current_thread;
    uint64_t va = (uint64_t)a_ring;
    int to_submit = (int)a_submit;
    if (to_submit < 0)
        return -EINVAL;
    if (to_submit > IORING_ENTRIES)
        to_submit = IORING_ENTRIES;

    /* Validate the whole ring page is user-RW, then reach it via the frame. */
    if (!vmm_user_range_ok(t->cr3, va & ~0xFFFull, PAGE_SIZE, 1))
        return -EFAULT;
    uint64_t phys = vmm_resolve(t->cr3, va & ~0xFFFull);
    if (!phys)
        return -EFAULT;
    struct io_ring *r = (struct io_ring *)(uintptr_t)(phys + (va & 0xFFFull));

    int done = 0;
    for (int i = 0; i < to_submit; i++) {
        int res = run_sqe(t, &r->sqes[i]);
        r->cqes[i].user_data = r->sqes[i].user_data;
        r->cqes[i].res = res;
        done++;
    }
    r->cq_tail = (uint32_t)done;
    return done;
}

void sys_io_uring_register(void) {
    syscall_register(SYS_IO_URING_SETUP, sys_io_uring_setup);
    syscall_register(SYS_IO_URING_ENTER, sys_io_uring_enter);
}
