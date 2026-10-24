/* kernel/proc/fd.c — per-process file-descriptor table (Phase 5b, ADR-022). */
#include "fd.h"
#include "sched.h"
#include "kheap.h"
#include "vfs.h"       /* struct vfs_file (deep copy in fd_clone) */
#include "pipe.h"      /* pipe_close / pipe_incref (FD_PIPE) */
#include "errno.h"     /* -ENOMEM */
#include "string.h"    /* memcpy */
#include "console.h"   /* kputs (fd_clone_selftest) */

void fd_table_init(struct fd_table *t) {
    for (int i = 0; i < FD_MAX; i++) {
        t->e[i].kind  = FD_NONE;
        t->e[i].off   = 0;
        t->e[i].mnt   = -1;
        t->e[i].file  = 0;
        t->e[i].pipe  = 0;
        t->e[i].epoll = 0;
        t->e[i].cap   = 0;
        t->e[i].flags = 0;
    }
}

void fd_init_std(struct fd_table *t) {
    /* stdin/stdout/stderr -> console. Console writes go straight to the kernel
     * console (the process's stdout), so no capability handle is needed here. */
    for (int i = 0; i < 3; i++) {
        t->e[i].kind = FD_CONSOLE;
        t->e[i].cap  = 0;
    }
}

int fd_alloc(struct tcb *t) {
    if (!t)
        return -1;
    for (int i = 0; i < FD_MAX; i++)
        if (t->fdt.e[i].kind == FD_NONE)
            return i;
    return -1;
}

struct fd_entry *fd_get(struct tcb *t, int fd) {
    if (!t || fd < 0 || fd >= FD_MAX)
        return 0;
    if (t->fdt.e[fd].kind == FD_NONE)
        return 0;
    return &t->fdt.e[fd];
}

void fd_free(struct tcb *t, int fd) {
    if (!t || fd < 0 || fd >= FD_MAX)
        return;
    struct fd_entry *e = &t->fdt.e[fd];
    if (e->kind == FD_PIPE && e->pipe) {
        /* DDR-787: drop the reference on THIS fd's end; frees once both are 0.
         * Dropping the last writer is what lets a blocked reader see EOF. */
        pipe_close(e->pipe, e->flags == PIPE_WRITE_END);
    } else if (e->kind == FD_EPOLL && e->epoll) {
        kfree(e->epoll);              /* sole owner (not inherited/dup'd) */
    } else if (e->file) {
        kfree(e->file);
    }
    e->file  = 0;
    e->pipe  = 0;
    e->epoll = 0;
    e->kind  = FD_NONE;
    e->off   = 0;
    e->mnt   = -1;
    e->cap   = 0;
    e->flags = 0;
}

/* Copy src's fd table into dst. On -ENOMEM, dst holds only references it owns
 * (pipe increfs on earlier slots) and untouched fresh slots; the caller releases
 * them with fd_free on every slot, as sched_destroy does. DDR-1184: a slot is
 * written only once its private vfs_file copy exists, so a failed allocation can
 * never leave dst holding src's pointer for that teardown to kfree. */
static int fd_clone_with(struct tcb *src, struct tcb *dst, void *(*alloc)(size_t)) {
    if (!src || !dst)
        return -1;
    for (int i = 0; i < FD_MAX; i++) {
        struct fd_entry *se = &src->fdt.e[i];
        struct fd_entry *de = &dst->fdt.e[i];
        struct vfs_file *nf = 0;
        *de = *se;                                   /* MUTANT M1: slot written before alloc */
        if (se->kind == FD_VFS && se->file) {
            nf = (struct vfs_file *)alloc(sizeof(struct vfs_file));
            if (!nf) {
                for (int j = 0; j < i; j++)          /* roll back earlier copies */
                    if (dst->fdt.e[j].kind == FD_VFS && dst->fdt.e[j].file) {
                        kfree(dst->fdt.e[j].file);
                        dst->fdt.e[j].file = 0;
                    }
                return -ENOMEM;
            }
            memcpy(nf, se->file, sizeof(struct vfs_file));
        }
        *de = *se;                                   /* scalar fields + (maybe) ptr */
        if (se->kind == FD_EPOLL) {
            de->kind = FD_NONE;                      /* epoll fds are not inherited */
            de->epoll = 0;
        } else if (se->kind == FD_PIPE && se->pipe) {
            /* DDR-787: the child's copy holds the same END as the parent's fd. */
            pipe_incref(se->pipe, se->flags == PIPE_WRITE_END);
        } else if (nf) {
            de->file = nf;                           /* private copy, not shared */
        }
    }
    return 0;
}

int fd_clone(struct tcb *src, struct tcb *dst) {
    return fd_clone_with(src, dst, kmalloc);
}

/* DDR-1184 gate (smoke-fdclone): fork's failure path, with the second vfs_file
 * copy failing. Boot-time, before the APs start, so no fork can race it; only
 * the two tcbs' fdt is touched. */
static int g_fdct_ok_allocs;
static void *fdct_alloc(size_t n) {
    return g_fdct_ok_allocs-- > 0 ? kmalloc(n) : 0;
}

void fd_clone_selftest(void) {
    struct tcb *p = (struct tcb *)kmalloc(sizeof(struct tcb));
    struct tcb *c = (struct tcb *)kmalloc(sizeof(struct tcb));
    if (!p || !c) {
        kputs("FDCLONE FAIL: no memory for the test tcbs\r\n");
        if (p) kfree(p);
        if (c) kfree(c);
        return;
    }
    fd_table_init(&p->fdt);
    fd_table_init(&c->fdt);
    for (int k = 3; k <= 5; k++) {
        struct vfs_file *f = (struct vfs_file *)kmalloc(sizeof(struct vfs_file));
        if (!f) {
            kputs("FDCLONE FAIL: no memory for the parent files\r\n");
            for (int i = 0; i < FD_MAX; i++) fd_free(p, i);
            kfree(p);
            kfree(c);
            return;
        }
        memset(f, 0x5A + k, sizeof *f);
        p->fdt.e[k].kind = FD_VFS;
        p->fdt.e[k].file = f;
    }
    uint64_t base = kheap_outstanding();
    g_fdct_ok_allocs = 1;                    /* slot 3 copies, slot 4 fails */
    int rc = fd_clone_with(p, c, fdct_alloc);
    for (int i = 0; i < FD_MAX; i++)         /* what sched_destroy does */
        fd_free(c, i);
    int intact = 1;
    for (int k = 3; k <= 5; k++) {
        const uint8_t *b = (const uint8_t *)p->fdt.e[k].file;
        for (size_t i = 0; i < sizeof(struct vfs_file); i++)
            if (b[i] != (uint8_t)(0x5A + k))
                intact = 0;
    }
    uint64_t after = kheap_outstanding();
    kputs(rc == -ENOMEM && intact ? "PRADYOS_FDCLONE_A enomem=1 parent_intact=1\r\n"
                                  : "PRADYOS_FDCLONE_A FAIL\r\n");
    if (after == base) {
        kputs("PRADYOS_FDCLONE_B leak=0\r\n");
    } else {
        kputs("PRADYOS_FDCLONE_B leak=1 delta=");
        kputhex(after - base);
        kputs("\r\n");
    }
    if (intact) {
        for (int i = 0; i < FD_MAX; i++)
            fd_free(p, i);
    } else {
        /* A parent file was already freed by the child's teardown: freeing it
         * again would panic (double free). Leak it and report. */
        kputs("FDCLONE FAIL: child teardown freed a parent file\r\n");
    }
    kfree(p);
    kfree(c);
}
