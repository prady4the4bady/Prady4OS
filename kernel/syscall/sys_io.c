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
#include "console.h"
#include "uaccess.h"
#include "errno.h"

static long sys_write(long fd, long ubuf, long count, long a4) {
    (void)a4;
    if (count < 0)
        return -EINVAL;
    if (count == 0)
        return 0;

    struct fd_entry *e = fd_get(current_thread, (int)fd);
    if (!e)
        return -EBADF;

    if (e->kind == FD_CONSOLE) {
        char kbuf[256];
        long total = 0, remaining = count;
        uint64_t uptr = (uint64_t)ubuf;
        while (remaining > 0) {
            size_t chunk = (remaining > (long)sizeof kbuf) ? sizeof kbuf : (size_t)remaining;
            if (copyin(kbuf, (const void __user *)(uintptr_t)uptr, chunk) < 0)
                return total > 0 ? total : -EFAULT;   /* partial write or EFAULT */
            for (size_t i = 0; i < chunk; i++)
                kputc(kbuf[i]);
            total     += (long)chunk;
            remaining -= (long)chunk;
            uptr      += chunk;
        }
        return total;
    }

    /* FD_VFS write arrives in slice 4. */
    return -EBADF;
}

static long sys_read(long fd, long ubuf, long count, long a4) {
    (void)ubuf; (void)count; (void)a4;
    struct fd_entry *e = fd_get(current_thread, (int)fd);
    if (!e)
        return -EBADF;
    return -ENOSYS;   /* real read (console + VFS) arrives in slice 4 */
}

void sys_io_register(void) {
    syscall_register(SYS_READ, sys_read);
    syscall_register(SYS_WRITE, sys_write);
}
