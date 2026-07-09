/* kernel/syscall/sys_io.c — sys_read / sys_write (Phase 5b slice 3, ADR-022).
 *
 * fd-based I/O over the per-process fd table. stdout/stderr (FD_CONSOLE) write
 * through the kernel console via the validated copyin path; a bad fd is -EBADF
 * and a bad user buffer is -EFAULT (the kernel never faults on the user buffer).
 * sys_read is a stub until the VFS read path lands in slice 4.
 */
#include "sys_io.h"
#include "syscall.h"
#include "sched.h"
#include "fd.h"
#include "vfs.h"
#include "pipe.h"
#include "console.h"
#include "uaccess.h"
#include "errno.h"

/* Write `count` (> 0) bytes from the user buffer at `uptr` to fd `e`, via the
 * validated copyin path (never a raw user dereference). Returns bytes written
 * (possibly short for a full pipe), or a negative errno when nothing was written.
 * Shared by sys_write and sys_writev (PROC-D). */
static long fd_write_user(struct fd_entry *e, uint64_t uptr, long count) {
    if (e->kind == FD_CONSOLE) {
        char kbuf[256];
        long total = 0, remaining = count;
        while (remaining > 0) {
            size_t chunk = (remaining > (long)sizeof kbuf) ? sizeof kbuf : (size_t)remaining;
            if (copyin(kbuf, (const void __user *)(uintptr_t)uptr, chunk) < 0)
                return total > 0 ? total : -EFAULT;   /* partial write or EFAULT */
            kwrite(kbuf, chunk);   /* whole chunk atomic vs. other CPUs' output */
            total     += (long)chunk;
            remaining -= (long)chunk;
            uptr      += chunk;
        }
        return total;
    }

    if (e->kind == FD_PIPE) {
        if (e->flags != PIPE_WRITE_END)
            return -EBADF;                        /* can't write the read end */
        char kbuf[256];
        long total = 0, remaining = count;
        while (remaining > 0) {
            size_t chunk = (remaining > (long)sizeof kbuf) ? sizeof kbuf : (size_t)remaining;
            if (copyin(kbuf, (const void __user *)(uintptr_t)uptr, chunk) < 0)
                return total > 0 ? total : -EFAULT;
            long w = pipe_write(e->pipe, kbuf, chunk);
            if (w <= 0)
                break;                            /* pipe full (non-blocking) */
            total += w; uptr += (uint64_t)w; remaining -= w;
            if (w < (long)chunk)
                break;
        }
        return total;
    }

    /* FD_VFS write arrives in slice 4. */
    return -EBADF;
}

static long sys_write(long fd, long ubuf, long count, long a4) {
    (void)a4;
    if (count < 0)
        return -EINVAL;
    if (count == 0)
        return 0;

    struct fd_entry *e = fd_get(current_thread, (int)fd);
    if (!e)
        return -EBADF;
    return fd_write_user(e, (uint64_t)ubuf, count);
}

/* sys_writev (PROC-D, ADR-023) — gather write over an iovec array. Each iovec's
 * base/len is validated by the fd_write_user copyin path; the array itself is
 * copied in (never raw-dereferenced). musl stdio issues this with 2 iovecs. */
#define SYS_IOV_MAX 16
struct user_iovec { uint64_t base; uint64_t len; };

static long sys_writev(long fd, long uiov, long iovcnt, long a4) {
    (void)a4;
    if (iovcnt < 0 || iovcnt > SYS_IOV_MAX)
        return -EINVAL;
    if (iovcnt == 0)
        return 0;

    struct fd_entry *e = fd_get(current_thread, (int)fd);
    if (!e)
        return -EBADF;

    struct user_iovec iov[SYS_IOV_MAX];
    if (copyin(iov, (const void __user *)(uintptr_t)uiov,
               (size_t)iovcnt * sizeof iov[0]) < 0)
        return -EFAULT;

    long total = 0;
    for (long i = 0; i < iovcnt; i++) {
        if (iov[i].len == 0)
            continue;
        if (iov[i].len > (uint64_t)__INT32_MAX__)     /* sane per-iovec bound */
            return total > 0 ? total : -EINVAL;
        long w = fd_write_user(e, iov[i].base, (long)iov[i].len);
        if (w < 0)
            return total > 0 ? total : w;             /* EFAULT/EBADF */
        total += w;
        if (w < (long)iov[i].len)
            break;                                    /* short write (pipe full) */
    }
    return total;
}

static long sys_read(long fd, long ubuf, long count, long a4) {
    (void)a4;
    if (count < 0)
        return -EINVAL;
    struct fd_entry *e = fd_get(current_thread, (int)fd);
    if (!e)
        return -EBADF;

    if (e->kind == FD_VFS && e->file) {
        if (count == 0)
            return 0;
        char kbuf[256];
        long total = 0, remaining = count;
        uint64_t uptr = (uint64_t)ubuf;
        while (remaining > 0) {
            uint32_t chunk = (remaining > (long)sizeof kbuf) ? (uint32_t)sizeof kbuf
                                                             : (uint32_t)remaining;
            int n = vfs_read(e->cap, e->file, e->off, kbuf, chunk);
            if (n < 0)
                return total > 0 ? total : -EIO;
            if (n == 0)
                break;                                  /* EOF */
            if (copyout((void __user *)(uintptr_t)uptr, kbuf, (size_t)n) < 0)
                return total > 0 ? total : -EFAULT;
            e->off    += (uint64_t)n;
            total     += n;
            uptr      += (uint64_t)n;
            remaining -= n;
            if (n < (int)chunk)
                break;                                  /* short read = EOF */
        }
        return total;
    }

    if (e->kind == FD_PIPE) {
        if (e->flags == PIPE_WRITE_END)
            return -EBADF;                        /* can't read the write end */
        if (count == 0)
            return 0;
        char kbuf[256];
        long total = 0, remaining = count;
        uint64_t uptr = (uint64_t)ubuf;
        while (remaining > 0) {
            uint64_t chunk = (remaining > (long)sizeof kbuf) ? sizeof kbuf : (uint64_t)remaining;
            long r = pipe_read(e->pipe, kbuf, chunk);
            if (r <= 0)
                break;                            /* empty (non-blocking baseline) */
            if (copyout((void __user *)(uintptr_t)uptr, kbuf, (size_t)r) < 0)
                return total > 0 ? total : -EFAULT;
            total += r; uptr += (uint64_t)r; remaining -= r;
            if (r < (long)chunk)
                break;
        }
        return total;
    }

    if (e->kind == FD_CONSOLE) {
        /* 5e (ADR-024 §D1): blocking console read. Poll COM1 RX, yielding while
         * empty so other threads run; return once >=1 byte arrives, draining any
         * already-buffered bytes up to `count`. No echo / line discipline here —
         * the shell owns the line. */
        if (count == 0)
            return 0;
        char kbuf[256];
        long n = 0;
        for (;;) {
            int c = kgetc_nb();
            if (c >= 0) { kbuf[n++] = (char)c; break; }
            yield();
        }
        while (n < count && n < (long)sizeof kbuf) {
            int c = kgetc_nb();
            if (c < 0)
                break;
            kbuf[n++] = (char)c;
        }
        if (copyout((void __user *)(uintptr_t)ubuf, kbuf, (size_t)n) < 0)
            return -EFAULT;
        return n;
    }

    return -EBADF;
}

void sys_io_register(void) {
    syscall_register(SYS_READ, sys_read);
    syscall_register(SYS_WRITE, sys_write);
    syscall_register(SYS_WRITEV, sys_writev);
}
