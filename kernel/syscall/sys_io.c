/* kernel/syscall/sys_io.c — sys_read / sys_write (Phase 5b slice 3, ADR-022).
 *
 * fd-based I/O over the per-process fd table. stdout/stderr (FD_CONSOLE) write
 * through the kernel console via the validated copyin path; a bad fd is -EBADF
 * and a bad user buffer is -EFAULT (the kernel never faults on the user buffer).
 * sys_read is a stub until the VFS read path lands in slice 4.
 */
#include "sys_io.h"
#include "sys_file.h"            /* DDR-782: O_APPEND (open flags, shared) */
#include "syscall.h"
#include "sched.h"
#include "fd.h"
#include "vfs.h"
#include "pipe.h"
#include "signal.h"              /* DDR-805: SIGPIPE on a readerless pipe write */
#include "console.h"
#include "uaccess.h"
#include "errno.h"
#include "pmm.h"                 /* DDR-764: 4 KiB bounce page for FD_VFS writes */

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
            /* DDR-787: block while the ring is full, instead of silently
             * truncating at PIPE_SIZE. Bounded (S2) by the reader count, never by
             * a timeout: the moment the last reader closes there is nobody to
             * drain the ring, so waiting could never succeed and we fail with
             * -EPIPE. yield() matches the FD_CONSOLE blocking read already in
             * this file. */
            while (pipe_full(e->pipe) && pipe_readers(e->pipe) > 0)
                yield();
            if (pipe_readers(e->pipe) <= 0) {
                /* DDR-805: no readers left, so this write can never complete.
                 * Raise SIGPIPE on ourselves — syscall context, own TCB, so no
                 * lock is taken and S6 does not apply. Delivery happens on the
                 * next IRQ return to ring 3, which is deliberate: a thread that
                 * installed a handler must still observe -EPIPE from this call,
                 * and terminating inside the syscall would make that case
                 * unrepresentable.
                 *
                 * ONLY on the nothing-written branch. A partial write SUCCEEDED
                 * in part; killing the writer would discard output it
                 * legitimately produced, and the short count already tells the
                 * caller the pipe is closing. */
                if (total > 0)
                    return total;
                current_thread->sig_pending |= (1ull << SIGPIPE);
                return -EPIPE;
            }
            long w = pipe_write(e->pipe, kbuf, chunk);
            if (w <= 0)
                break;                            /* no progress possible */
            total += w; uptr += (uint64_t)w; remaining -= w;
            if (w < (long)chunk) {
                /* Partial: the ring filled mid-chunk. Re-copy the remainder on
                 * the next iteration rather than dropping it (the old bug). */
                continue;
            }
        }
        return total;
    }

    /* DDR-744/764: FD_VFS write — persist to the backing file through the
     * cap-gated VFS. Chunked copyin, advancing the fd offset by the bytes actually
     * written. vfs_write enforces CAP_FS_WRITE + the per-process write budget; a
     * backend short-write ends the loop (returns the partial). DDR-764: the chunk
     * is one 4 KiB block (from a PMM page, not the 16 KiB kernel stack) — 16x
     * fewer vfs_write iterations, and each SFS extent now holds a full block so a
     * ring-3 SFS file reaches 4 extents * 4 KiB = 16 KiB instead of ~1 KiB. */
    if (e->kind == FD_VFS && e->file) {
        /* DDR-782: O_APPEND. Re-position at end-of-file once per write() call,
         * before the chunk loop — that is the POSIX atomicity property (the whole
         * call lands at the end as one act), and it is what an lseek-at-open
         * append cannot give when writers share a file. */
        if (e->flags & O_APPEND)
            e->off = e->file->size;
        uint64_t kp = pmm_alloc_page();
        if (!kp)
            return -ENOMEM;
        char *kbuf = (char *)(uintptr_t)kp;
        const uint32_t CHUNK = 4096;
        long total = 0, remaining = count;
        while (remaining > 0) {
            uint32_t chunk = (remaining > (long)CHUNK) ? CHUNK : (uint32_t)remaining;
            if (copyin(kbuf, (const void __user *)(uintptr_t)uptr, chunk) < 0) {
                pmm_free_page(kp);
                return total > 0 ? total : -EFAULT;
            }
            int w = vfs_write(e->cap, e->file, e->off, kbuf, chunk);
            if (w < 0) {
                pmm_free_page(kp);
                return total > 0 ? total : -EIO;
            }
            if (w == 0)
                break;                                  /* budget/space exhausted */
            e->off    += (uint64_t)w;
            total     += w;
            uptr      += (uint64_t)w;
            remaining -= w;
            if (w < (int)chunk)
                break;                                  /* short write */
        }
        pmm_free_page(kp);
        return total;
    }

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
            /* DDR-787: block while the ring is empty and a writer still exists,
             * instead of reporting EOF the instant it happens to be empty — that
             * made `a | b` timing-dependent, so b could print nothing if it was
             * scheduled first. Bounded (S2) by the writer count: once the last
             * writer closes, nothing can ever arrive, and an empty ring is a
             * genuine EOF. Only wait when we have delivered nothing yet, so a
             * partially-satisfied read returns promptly. */
            if (total == 0)
                while (!pipe_has_data(e->pipe) && pipe_writers(e->pipe) > 0)
                    yield();
            long r = pipe_read(e->pipe, kbuf, chunk);
            if (r <= 0)
                break;                            /* empty + no writers = EOF */
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

/* SYS_DMESG (DDR-750): copy the recent kernel log to a user buffer. No cap
 * (diagnostic, like the other introspection syscalls). Capped at 4 KiB per call;
 * staged into a kernel buffer under klog's lock, then copyout (never a user
 * fault under the log lock). */
static long sys_dmesg(long ubuf, long max, long a3, long a4) {
    (void)a3; (void)a4;
    if (max <= 0)
        return 0;
    char kbuf[4096];
    uint32_t want = (max > (long)sizeof kbuf) ? (uint32_t)sizeof kbuf : (uint32_t)max;
    uint32_t n = klog_read(kbuf, want);
    if (n && copyout((void __user *)(uintptr_t)ubuf, kbuf, n) < 0)
        return -EFAULT;
    return (long)n;
}

void sys_io_register(void) {
    syscall_register(SYS_READ, sys_read);
    syscall_register(SYS_WRITE, sys_write);
    syscall_register(SYS_WRITEV, sys_writev);
    syscall_register(SYS_DMESG, sys_dmesg);      /* DDR-750 */
}
