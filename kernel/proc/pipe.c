/* kernel/proc/pipe.c — anonymous pipes + dup2 (Phase 5b, PROC-A).
 *
 * SYS_PIPE installs a read-end and a write-end fd over one shared byte ring;
 * SYS_DUP2 duplicates an fd onto a chosen number (sharing the same pipe / a
 * private vfs_file copy). The ring uses a power-of-two size and a mask index
 * (head - tail = bytes buffered), so no modulo. Non-blocking baseline.
 */
#include "pipe.h"
#include "console.h"     /* DDR-790: kputs/kputhex/kputdec for the destroy trace */
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
    /* DDR-787: counted per END, not one refcount. A blocked reader needs to know
     * a writer still exists (else it waits forever), and vice versa — that
     * condition is what bounds the waiting loops in sys_read/sys_write (S2). */
    int      readers;      /* read-end fds referencing this pipe      */
    int      writers;      /* write-end fds referencing this pipe     */
};

struct pipe *pipe_create(void) {
    struct pipe *p = (struct pipe *)kmalloc(sizeof *p);
    if (!p)
        return 0;
    p->buf = pmm_alloc_page();
    if (!p->buf) { kfree(p); return 0; }
    p->head = p->tail = 0;
    p->readers = p->writers = 0;
#if PIPE_TRACE
    /* DDR-790: paired with the destroy trace so create/destroy can be matched.
     * Without this, "same pointer freed twice" is ambiguous — kheap recycles
     * addresses, so it may be two different pipes rather than a double free. */
    kputs("[pipe] create  p="); kputhex((uint64_t)(uintptr_t)p); kputs("\r\n");
#endif
    return p;
}

void pipe_destroy(struct pipe *p) {
    if (!p)
        return;
#if PIPE_TRACE
    /* DDR-790: name every pipe we free. CI run 30215987521 panicked with
     * "kfree: double free objsize=0x20" — the bucket struct pipe lands in — and
     * inspection of the DDR-787 refcount sites did not find the defect, so the
     * next occurrence must be decidable rather than argued: if the panicking
     * pointer matches a line below, it IS the pipe (and r/w say how it got
     * there); if it never appears, the pipe is exonerated. Evidence only — no
     * gate asserts on this.
     *
     * OPT-IN (-DPIPE_TRACE=1): run 30303017178 showed why it cannot be
     * unconditional. smoke-dmesg writes a marker then reads back the LAST 4 KiB
     * of the log ring; a per-pipe trace pushes enough bytes in between to evict
     * the marker, so the diagnostic broke an unrelated gate. Enable it when
     * chasing the panic, not by default. */
    kputs("[pipe] destroy p="); kputhex((uint64_t)(uintptr_t)p);   /* kputhex adds "0x" */
    kputs(" r="); kputdec((uint64_t)(p->readers < 0 ? 0 : p->readers));
    kputs(" w="); kputdec((uint64_t)(p->writers < 0 ? 0 : p->writers));
    kputs("\r\n");
#endif
    if (p->buf)
        pmm_free_page(p->buf);
    kfree(p);
}

void pipe_incref(struct pipe *p, int is_write) {
    if (!p)
        return;
    if (is_write) p->writers++;
    else          p->readers++;
}

void pipe_close(struct pipe *p, int is_write) {
    if (!p)
        return;
    /* DDR-790: free only if THIS close actually dropped a reference. The first
     * cut freed whenever both counts read 0, so a close that decremented nothing
     * — because the count was already 0 — freed the pipe a second time. The
     * pre-DDR-787 single refcount hid that by going negative; splitting the
     * counts and clamping at 0 turned a benign double-decrement into a double
     * kfree, which is the panic in CI run 30215987521. Requiring a real
     * transition makes a duplicate or stale close a no-op. */
    int dropped = 0;
    if (is_write) { if (p->writers > 0) { p->writers--; dropped = 1; } }
    else          { if (p->readers > 0) { p->readers--; dropped = 1; } }
    /* Free only once BOTH ends are gone. A peer blocked in read/write wakes on
     * the count reaching 0 (EOF / -EPIPE) and must not touch a freed pipe. */
    if (dropped && p->readers <= 0 && p->writers <= 0)
        pipe_destroy(p);
}

int pipe_writers(struct pipe *p) { return p ? p->writers : 0; }
int pipe_readers(struct pipe *p) { return p ? p->readers : 0; }
int pipe_full(struct pipe *p) {
    return p && (uint32_t)(p->head - p->tail) >= PIPE_SIZE;
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
    if (rfd < 0) { pipe_destroy(p); return -EMFILE; }   /* never installed */
    install(&t->fdt.e[rfd], p, 0);          /* read end */
    pipe_incref(p, 0);

    int wfd = fd_alloc(t);
    if (wfd < 0) { fd_free(t, rfd); return -EMFILE; }   /* fd_free -> pipe_close */
    install(&t->fdt.e[wfd], p, PIPE_WRITE_END);
    pipe_incref(p, 1);

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
        /* DDR-787: the dup'd fd inherits its END from the source's flags, so it
         * must be counted on that same side. */
        pipe_incref(ne->pipe, ne->flags == PIPE_WRITE_END);
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
