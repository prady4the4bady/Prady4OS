/* kernel/proc/pipe.c — anonymous pipes + dup2 (Phase 5b, PROC-A).
 *
 * SYS_PIPE installs a read-end and a write-end fd over one shared byte ring;
 * SYS_DUP2 duplicates an fd onto a chosen number (sharing the same pipe / a
 * private vfs_file copy). The ring uses a power-of-two size and a mask index
 * (head - tail = bytes buffered), so no modulo. Non-blocking baseline.
 */
#include "pipe.h"
#include "sched.h"
#include "fd.h"
#include "vfs.h"          /* struct vfs_file (dup2 of an FD_VFS) */
#include "syscall.h"
#include "uaccess.h"
#include "kheap.h"
#include "pmm.h"
#include "string.h"
#include "errno.h"

#define PIPE_SIZE 4096u
#define PIPE_MASK (PIPE_SIZE - 1u)
_Static_assert((PIPE_SIZE & PIPE_MASK) == 0, "PIPE_SIZE must be a power of two");

struct pipe {
    uint64_t buf;          /* 4 KiB ring (pmm frame, identity-mapped) */
    uint32_t head;         /* total bytes written (masked on access)  */
    uint32_t tail;         /* total bytes read                        */
    int      refcount;     /* fds referencing this pipe               */
};

struct pipe *pipe_create(void) {
    struct pipe *p = (struct pipe *)kmalloc(sizeof *p);
    if (!p)
        return 0;
    p->buf = pmm_alloc_page();
    if (!p->buf) { kfree(p); return 0; }
    p->head = p->tail = 0;
    p->refcount = 0;
    return p;
}

void pipe_incref(struct pipe *p) {
    if (p)
        p->refcount++;
}

void pipe_close(struct pipe *p) {
    if (!p)
        return;
    if (--p->refcount <= 0) {
        if (p->buf)
            pmm_free_page(p->buf);
        kfree(p);
    }
}

long pipe_write(struct pipe *p, const void *src, uint64_t n) {
    if (!p)
        return -1;
    uint8_t *ring = (uint8_t *)(uintptr_t)p->buf;
    const uint8_t *s = (const uint8_t *)src;
    uint64_t i = 0;
    while (i < n && (uint32_t)(p->head - p->tail) < PIPE_SIZE)
        ring[p->head++ & PIPE_MASK] = s[i++];
    return (long)i;
}

long pipe_read(struct pipe *p, void *dst, uint64_t n) {
    if (!p)
        return -1;
    uint8_t *ring = (uint8_t *)(uintptr_t)p->buf;
    uint8_t *d = (uint8_t *)dst;
    uint64_t i = 0;
    while (i < n && p->tail != p->head)
        d[i++] = ring[p->tail++ & PIPE_MASK];
    return (long)i;
}

int pipe_has_data(struct pipe *p) {
    return p && p->head != p->tail;
}

/* --- syscalls ------------------------------------------------------------- */

static void install(struct fd_entry *e, struct pipe *p, int flags) {
    e->kind  = FD_PIPE;
    e->pipe  = p;
    e->flags = flags;
    e->off   = 0;
    e->mnt   = -1;
    e->cap   = 0;
    e->file  = 0;
}

static long sys_pipe(long ufds, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    struct tcb *t = current_thread;

    struct pipe *p = pipe_create();
    if (!p)
        return -ENOMEM;

    int rfd = fd_alloc(t);
    if (rfd < 0) { pipe_close(p); return -EMFILE; }
    install(&t->fdt.e[rfd], p, 0);          /* read end */
    pipe_incref(p);

    int wfd = fd_alloc(t);
    if (wfd < 0) { fd_free(t, rfd); return -EMFILE; }   /* fd_free -> pipe_close */
    install(&t->fdt.e[wfd], p, PIPE_WRITE_END);
    pipe_incref(p);

    int fds[2] = { rfd, wfd };
    if (copyout((void __user *)(uintptr_t)ufds, fds, sizeof fds) < 0) {
        fd_free(t, rfd);
        fd_free(t, wfd);
        return -EFAULT;
    }
    return 0;
}

static long sys_dup2(long a_old, long a_new, long a3, long a4) {
    (void)a3; (void)a4;
    struct tcb *t = current_thread;
    int oldfd = (int)a_old, newfd = (int)a_new;

    struct fd_entry *oe = fd_get(t, oldfd);
    if (!oe || newfd < 0 || newfd >= FD_MAX)
        return -EBADF;
    if (oldfd == newfd)
        return newfd;                       /* dup onto itself: no-op */

    if (t->fdt.e[newfd].kind != FD_NONE)
        fd_free(t, newfd);                  /* close the target first */

    struct fd_entry *ne = &t->fdt.e[newfd];
    *ne = *oe;                              /* copy kind/flags/off/mnt/cap/ptrs */
    if (ne->kind == FD_PIPE && ne->pipe) {
        pipe_incref(ne->pipe);             /* share the same pipe */
    } else if (ne->kind == FD_VFS && ne->file) {
        struct vfs_file *nf = (struct vfs_file *)kmalloc(sizeof(struct vfs_file));
        if (!nf) { ne->kind = FD_NONE; ne->file = 0; return -ENOMEM; }
        memcpy(nf, oe->file, sizeof(struct vfs_file));
        ne->file = nf;                     /* independent open-file copy */
    }
    return newfd;
}

void pipe_register(void) {
    syscall_register(SYS_PIPE, sys_pipe);
    syscall_register(SYS_DUP2, sys_dup2);
}
