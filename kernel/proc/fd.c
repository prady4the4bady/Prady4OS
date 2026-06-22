/* kernel/proc/fd.c — per-process file-descriptor table (Phase 5b, ADR-022). */
#include "fd.h"
#include "sched.h"
#include "kheap.h"
#include "vfs.h"       /* struct vfs_file (deep copy in fd_clone) */
#include "pipe.h"      /* pipe_close / pipe_incref (FD_PIPE) */
#include "errno.h"     /* -ENOMEM */
#include "string.h"    /* memcpy */

void fd_table_init(struct fd_table *t) {
    for (int i = 0; i < FD_MAX; i++) {
        t->e[i].kind  = FD_NONE;
        t->e[i].off   = 0;
        t->e[i].mnt   = -1;
        t->e[i].file  = 0;
        t->e[i].pipe  = 0;
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
        pipe_close(e->pipe);          /* drop a reference; frees at 0 */
    } else if (e->file) {
        kfree(e->file);
    }
    e->file  = 0;
    e->pipe  = 0;
    e->kind  = FD_NONE;
    e->off   = 0;
    e->mnt   = -1;
    e->cap   = 0;
    e->flags = 0;
}

int fd_clone(struct tcb *src, struct tcb *dst) {
    if (!src || !dst)
        return -1;
    for (int i = 0; i < FD_MAX; i++) {
        struct fd_entry *se = &src->fdt.e[i];
        struct fd_entry *de = &dst->fdt.e[i];
        *de = *se;                                   /* scalar fields + (maybe) ptr */
        if (se->kind == FD_PIPE && se->pipe) {
            pipe_incref(se->pipe);                   /* child shares the same pipe */
        } else if (se->kind == FD_VFS && se->file) {
            struct vfs_file *nf = (struct vfs_file *)kmalloc(sizeof(struct vfs_file));
            if (!nf) {
                for (int j = 0; j < i; j++)          /* roll back earlier copies */
                    if (dst->fdt.e[j].kind == FD_VFS && dst->fdt.e[j].file) {
                        kfree(dst->fdt.e[j].file);
                        dst->fdt.e[j].file = 0;
                    }
                return -ENOMEM;
            }
            memcpy(nf, se->file, sizeof(struct vfs_file));
            de->file = nf;                           /* private copy, not shared */
        }
    }
    return 0;
}
