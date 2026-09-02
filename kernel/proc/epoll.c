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
#include "irq.h"        /* DDR-1037: g_ticks, for poll()'s timeout deadline */

#define EPOLL_MAX        64
#define EPOLLIN          0x001u
/* DDR-1037: POSIX poll() bits. IN/OUT/ERR/HUP share the EPOLL values by design
 * on Linux, which is what lets fd_ready_mask serve both callers. */
#define POLLIN           0x001u
#define POLLOUT          0x004u
#define POLLERR          0x008u
#define POLLHUP          0x010u
#define POLLNVAL         0x020u
#define POLL_MAX_FDS     32
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

/* DDR-1037: the kernel's ONE readiness predicate, shared by epoll_wait and poll.
 *
 * Two predicates that can disagree about the same fd is the drift this codebase
 * guards against elsewhere, so poll() does not get a parallel copy -- it calls
 * this. Returns a POSIX-shaped mask (POLLIN/POLLOUT/POLLHUP); the EPOLL bit
 * values coincide with the POLL ones for IN/OUT/ERR/HUP, which is why one mask
 * serves both callers.
 *
 * `revents` is NOT filtered by `events` here: POSIX says POLLHUP/POLLERR/POLLNVAL
 * are reported whether or not they were requested. Callers that want the
 * requested-only view mask it themselves. */
static uint32_t fd_ready_mask(struct tcb *t, int fd) {
    struct fd_entry *e = fd_get(t, fd);
    if (!e)
        return 0;
    switch (e->kind) {
    case FD_PIPE:
        if (e->flags == PIPE_WRITE_END)
            return (pipe_full(e->pipe) ? 0u : POLLOUT)
                 | (pipe_readers(e->pipe) ? 0u : POLLERR);
        return (pipe_has_data(e->pipe) ? POLLIN : 0u)
             | (pipe_writers(e->pipe) ? 0u : POLLHUP);
    case FD_VFS:
        /* POSIX: a regular file NEVER blocks, so it is always ready both ways.
         * The pre-DDR-1037 predicate returned 0 here, which made epoll_wait on a
         * file fd answer WRONGLY rather than narrowly -- a correctness fix, not
         * an extension. */
        return POLLIN | POLLOUT;
    case FD_CONSOLE:
        /* Writable always. NOT readable-reportable: this kernel has no
         * console-input predicate under any name, so stdin can only ever be
         * reported not-ready. Saying otherwise would be a guess (DDR-1037 §3). */
        return POLLOUT;
    default:
        return 0;                    /* FD_EPOLL and anything else: not pollable */
    }
}

/* epoll's view: the requested bits only, in EPOLL terms. */
static uint32_t fd_ready(struct tcb *t, int fd, uint32_t events) {
    return fd_ready_mask(t, fd) & events;
}

static long sys_epoll_create(long a_size, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
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

static long sys_epoll_ctl(long a_epfd, long a_op, long a_fd, long a_ev, long a5, long a6) {
    (void)a5; (void)a6;
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

static long sys_epoll_wait(long a_epfd, long a_events, long a_max, long a_timeout, long a5, long a6) {
    (void)a5; (void)a6;
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

/* DDR-1037: POSIX poll(). Lives here, beside epoll, so both share one readiness
 * predicate rather than keeping two that can disagree. */
struct pollfd {
    int   fd;
    short events;
    short revents;
} __attribute__((packed));
_Static_assert(sizeof(struct pollfd) == 8, "pollfd layout");

static long sys_poll(long a_fds, long a_nfds, long a_timeout, long a4, long a5, long a6) {
    (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_thread;
    if (a_nfds < 0 || a_nfds > POLL_MAX_FDS)
        return -EINVAL;
    unsigned nfds = (unsigned)a_nfds;
    if (nfds == 0)
        return 0;

    struct pollfd fds[POLL_MAX_FDS];
    if (copyin(fds, (const void __user *)(uintptr_t)a_fds, nfds * sizeof fds[0]) < 0)
        return -EFAULT;

    /* The deadline is computed ONCE, before the loop: recomputing it per pass
     * would make a timeout that never expires (each pass resetting its own
     * deadline) -- the shape DDR-1037 §5 asks arm E to catch. */
    int      timeout = (int)a_timeout;
    uint64_t deadline = (timeout > 0)
        ? g_ticks + ((uint64_t)timeout / 10u) + 1u   /* ms -> 100 Hz ticks */
        : 0;

    for (;;) {
        int ready = 0;
        for (unsigned i = 0; i < nfds; i++) {
            if (fds[i].fd < 0) {                 /* POSIX: ignored, revents 0 */
                fds[i].revents = 0;
                continue;
            }
            if (!fd_get(t, fds[i].fd)) {         /* POSIX: bad fd is POLLNVAL */
                fds[i].revents = (short)POLLNVAL;
                ready++;
                continue;
            }
            uint32_t m = fd_ready_mask(t, fds[i].fd);
            /* POLLHUP/POLLERR/POLLNVAL are reported unrequested (POSIX); the
             * readable/writable bits are filtered to what was asked for. */
            uint32_t want = (uint32_t)(unsigned short)fds[i].events
                          | POLLERR | POLLHUP;
            fds[i].revents = (short)(m & want);
            if (fds[i].revents)
                ready++;
        }
        if (ready > 0 || timeout == 0)
            break;
        if (timeout > 0 && g_ticks >= deadline)
            break;                               /* expired: return 0 */
        /* timeout < 0 blocks until something is ready. That is an unbounded
         * wait, and DDR-1037 §4 says so rather than burying it: the difference
         * from DDR-994's defect class is that blocking forever is the caller's
         * explicit request, and yield() carries an interrupt window since
         * DDR-981, so the CPU is not wedged. */
        yield();
    }

    int n = 0;
    for (unsigned i = 0; i < nfds; i++)
        if (fds[i].revents)
            n++;
    if (copyout((void __user *)(uintptr_t)a_fds, fds, nfds * sizeof fds[0]) < 0)
        return -EFAULT;
    return n;
}

void epoll_register(void) {
    syscall_register(SYS_POLL, sys_poll);           /* DDR-1037 */
    syscall_register(SYS_EPOLL_CREATE, sys_epoll_create);
    syscall_register(SYS_EPOLL_CTL,    sys_epoll_ctl);
    syscall_register(SYS_EPOLL_WAIT,   sys_epoll_wait);
}
