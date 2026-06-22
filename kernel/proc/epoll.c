/* kernel/proc/epoll.c — minimal epoll (Phase 5b, PROC-B).
 *
 * SYS_EPOLL_CREATE installs an FD_EPOLL fd holding a fixed interest table;
 * SYS_EPOLL_CTL adds/modifies/removes (fd, events, data) entries; SYS_EPOLL_WAIT
 * polls each watched fd's readiness and copies out the ready events. Readiness:
 * EPOLLIN on a pipe read-end iff the ring has buffered bytes (non-blocking).
 */
#include "epoll.h"
#include "sched.h"
#include "fd.h"
#include "pipe.h"
#include "syscall.h"
#include "uaccess.h"
#include "kheap.h"
#include "errno.h"

#define EPOLL_MAX        64
#define EPOLLIN          0x001u
#define EPOLL_CTL_ADD    1
#define EPOLL_CTL_DEL    2
#define EPOLL_CTL_MOD    3

/* Linux-compatible user event record (packed: 4-byte events + 8-byte data). */
struct epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));
_Static_assert(sizeof(struct epoll_event) == 12, "epoll_event layout");

struct epoll_item {
    int      fd;
    uint32_t events;
    uint64_t data;
    int      active;
};

struct epoll {
    struct epoll_item items[EPOLL_MAX];
};

static struct epoll *epoll_of(struct tcb *t, int epfd) {
    struct fd_entry *e = fd_get(t, epfd);
    if (!e || e->kind != FD_EPOLL)
        return 0;
    return e->epoll;
}

/* Is `fd` currently ready for the requested events? Baseline: EPOLLIN on a pipe
 * read-end with buffered data. */
static uint32_t fd_ready(struct tcb *t, int fd, uint32_t events) {
    struct fd_entry *e = fd_get(t, fd);
    if (!e)
        return 0;
    if ((events & EPOLLIN) && e->kind == FD_PIPE && e->flags != PIPE_WRITE_END
        && pipe_has_data(e->pipe))
        return EPOLLIN;
    return 0;
}

static long sys_epoll_create(long a_size, long a2, long a3, long a4) {
    (void)a_size; (void)a2; (void)a3; (void)a4;
    struct tcb *t = current_thread;
    int fd = fd_alloc(t);
    if (fd < 0)
        return -EMFILE;
    struct epoll *ep = (struct epoll *)kmalloc(sizeof *ep);
    if (!ep)
        return -ENOMEM;
    for (int i = 0; i < EPOLL_MAX; i++)
        ep->items[i].active = 0;
    struct fd_entry *e = &t->fdt.e[fd];
    e->kind = FD_EPOLL; e->epoll = ep; e->file = 0; e->pipe = 0;
    e->off = 0; e->mnt = -1; e->cap = 0; e->flags = 0;
    return fd;
}

static long sys_epoll_ctl(long a_epfd, long a_op, long a_fd, long a_ev) {
    struct tcb *t = current_thread;
    struct epoll *ep = epoll_of(t, (int)a_epfd);
    if (!ep)
        return -EBADF;
    int op = (int)a_op, fd = (int)a_fd;
    if (!fd_get(t, fd))
        return -EBADF;

    struct epoll_event ev = { 0, 0 };
    if (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) {
        if (copyin(&ev, (const void __user *)(uintptr_t)a_ev, sizeof ev) < 0)
            return -EFAULT;
    }

    if (op == EPOLL_CTL_ADD) {
        int slot = -1;
        for (int i = 0; i < EPOLL_MAX; i++) {
            if (ep->items[i].active && ep->items[i].fd == fd)
                return -EEXIST;
            if (slot < 0 && !ep->items[i].active)
                slot = i;
        }
        if (slot < 0)
            return -ENOMEM;
        ep->items[slot].fd = fd;
        ep->items[slot].events = ev.events;
        ep->items[slot].data = ev.data;
        ep->items[slot].active = 1;
        return 0;
    }
    for (int i = 0; i < EPOLL_MAX; i++) {
        if (ep->items[i].active && ep->items[i].fd == fd) {
            if (op == EPOLL_CTL_DEL) {
                ep->items[i].active = 0;
            } else if (op == EPOLL_CTL_MOD) {
                ep->items[i].events = ev.events;
                ep->items[i].data = ev.data;
            } else {
                return -EINVAL;
            }
            return 0;
        }
    }
    return -ENOENT;
}

static long sys_epoll_wait(long a_epfd, long a_events, long a_max, long a_timeout) {
    (void)a_timeout;                 /* non-blocking baseline */
    struct tcb *t = current_thread;
    struct epoll *ep = epoll_of(t, (int)a_epfd);
    if (!ep)
        return -EBADF;
    int max = (int)a_max;
    if (max <= 0)
        return -EINVAL;

    uint64_t uptr = (uint64_t)a_events;
    int n = 0;
    for (int i = 0; i < EPOLL_MAX && n < max; i++) {
        if (!ep->items[i].active)
            continue;
        uint32_t r = fd_ready(t, ep->items[i].fd, ep->items[i].events);
        if (!r)
            continue;
        struct epoll_event out = { r, ep->items[i].data };
        if (copyout((void __user *)(uintptr_t)uptr, &out, sizeof out) < 0)
            return n > 0 ? n : -EFAULT;
        uptr += sizeof out;
        n++;
    }
    return n;
}

void epoll_register(void) {
    syscall_register(SYS_EPOLL_CREATE, sys_epoll_create);
    syscall_register(SYS_EPOLL_CTL,    sys_epoll_ctl);
    syscall_register(SYS_EPOLL_WAIT,   sys_epoll_wait);
}
