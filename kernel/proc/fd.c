/* kernel/proc/fd.c — per-process file-descriptor table (Phase 5b, ADR-022). */
#include "fd.h"
#include "sched.h"
#include "kheap.h"

void fd_table_init(struct fd_table *t) {
    for (int i = 0; i < FD_MAX; i++) {
        t->e[i].kind  = FD_NONE;
        t->e[i].off   = 0;
        t->e[i].mnt   = -1;
        t->e[i].file  = 0;
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
    if (e->file) {
        kfree(e->file);
        e->file = 0;
    }
    e->kind  = FD_NONE;
    e->off   = 0;
    e->mnt   = -1;
    e->cap   = 0;
    e->flags = 0;
}
