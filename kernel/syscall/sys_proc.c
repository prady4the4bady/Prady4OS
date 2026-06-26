/* kernel/syscall/sys_proc.c — sys_lseek / sys_getcwd (Phase 5b slice 5, ADR-022).
 *
 * lseek repositions a VFS fd; getcwd returns the process cwd, which is "/" until
 * a real working-directory / mount-point namespace exists (deferred, see
 * docs/build_status.md). sys_getpid is unchanged in syscall.c (SYS_GETPID).
 */
#include "sys_proc.h"
#include "syscall.h"
#include "sched.h"
#include "fd.h"
#include "vfs.h"
#include "uaccess.h"
#include "errno.h"
#include "vmm.h"               /* VMM_USER_MIN/MAX for fs_base validation */
#include "cpu_mitigations.h"   /* cpu_wrmsr + MSR_IA32_FS_BASE (PROC-D)   */

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

static long sys_lseek(long fd, long offset, long whence, long a4) {
    (void)a4;
    struct fd_entry *e = fd_get(current_thread, (int)fd);
    if (!e)
        return -EBADF;
    if (e->kind != FD_VFS || !e->file)
        return -ESPIPE;                       /* console / non-seekable */

    int64_t base;
    switch (whence) {
        case SEEK_SET: base = 0;                          break;
        case SEEK_CUR: base = (int64_t)e->off;            break;
        case SEEK_END: base = (int64_t)e->file->size;     break;
        default:       return -EINVAL;
    }
    int64_t pos = base + (int64_t)offset;
    if (pos < 0)
        return -EINVAL;
    e->off = (uint64_t)pos;
    return (long)pos;
}

static long sys_getcwd(long ubuf, long size, long a3, long a4) {
    (void)a3; (void)a4;
    static const char cwd[] = "/";
    size_t need = sizeof cwd;                  /* 2: '/' + NUL */
    if (size < 0 || (uint64_t)size < need)
        return -ERANGE;
    if (copyout((void __user *)(uintptr_t)ubuf, cwd, need) < 0)
        return -EFAULT;
    return (long)need;                         /* Linux getcwd returns length incl. NUL */
}

/* sys_set_tls (PROC-D, ADR-023) — set the calling thread's pointer (FS base).
 * The analog of Linux arch_prctl(ARCH_SET_FS): record it in the TCB (authority
 * for switch-in restore) and program IA32_FS_BASE for the current run. The base
 * must be 0 or a user-range address — ring 3 may never aim %fs at kernel space. */
static long sys_set_tls(long fs_base, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    uint64_t fb = (uint64_t)fs_base;
    if (fb != 0 && (fb < VMM_USER_MIN || fb >= VMM_USER_MAX))
        return -EINVAL;
    current_thread->fs_base = fb;
    cpu_wrmsr(MSR_IA32_FS_BASE, fb);
    return 0;
}

void sys_proc_register(void) {
    syscall_register(SYS_LSEEK,   sys_lseek);
    syscall_register(SYS_GETCWD,  sys_getcwd);
    syscall_register(SYS_SET_TLS, sys_set_tls);
}
