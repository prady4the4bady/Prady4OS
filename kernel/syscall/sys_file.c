/* kernel/syscall/sys_file.c — sys_open / sys_close / sys_fstat (Phase 5b slice 4).
 *
 * Bridges the POSIX fd API to the capability-gated VFS. The process is granted
 * an FS capability + a root mount at load time (elf_load); open resolves the
 * (copyin'd) path against that mount, allocates an fd, and records the open file
 * + the authorizing capability in the fd entry. read/lseek/fstat then operate on
 * the fd, so NCS enforcement survives under the POSIX surface (ADR-022).
 *
 * Path resolution is single-root for 5b (the process root mount); a full
 * mount-point namespace is deferred (docs/build_status.md DEFERRED).
 */
#include "sys_file.h"
#include "syscall.h"
#include "sched.h"
#include "fd.h"
#include "vfs.h"
#include "stat.h"
#include "uaccess.h"
#include "errno.h"
#include "kheap.h"
#include "string.h"

#define PATH_MAX 256

static long sys_open(long upath, long flags, long mode, long a4) {
    (void)mode; (void)a4;
    struct tcb *t = current_thread;
    if (t->root_mnt < 0)
        return -ENOENT;                       /* no filesystem available */

    char path[PATH_MAX];
    if (copyinstr(path, (const void __user *)(uintptr_t)upath, sizeof path, 0) < 0)
        return -EFAULT;

    int fd = fd_alloc(t);
    if (fd < 0)
        return -EMFILE;

    struct vfs_file *f = (struct vfs_file *)kmalloc(sizeof(struct vfs_file));
    if (!f)
        return -ENOMEM;

    if (vfs_open(t->fs_cap, t->root_mnt, path, f) != 0) {
        kfree(f);
        return -ENOENT;
    }

    struct fd_entry *e = &t->fdt.e[fd];
    e->kind  = FD_VFS;
    e->file  = f;
    e->off   = 0;
    e->mnt   = t->root_mnt;
    e->cap   = t->fs_cap;
    e->flags = (int)flags;
    return fd;
}

static long sys_close(long fd, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    if (!fd_get(current_thread, (int)fd))
        return -EBADF;
    fd_free(current_thread, (int)fd);         /* frees the kmalloc'd vfs_file */
    return 0;
}

static long sys_fstat(long fd, long ustat, long a3, long a4) {
    (void)a3; (void)a4;
    struct fd_entry *e = fd_get(current_thread, (int)fd);
    if (!e)
        return -EBADF;

    struct stat st;
    memset(&st, 0, sizeof st);
    if (e->kind == FD_VFS && e->file) {
        st.st_size    = (int64_t)e->file->size;
        st.st_mode    = S_IFREG | 0644;
        st.st_nlink   = 1;
        st.st_blksize = 4096;
        st.st_blocks  = (int64_t)((e->file->size + 511) / 512);
    } else {
        st.st_mode = S_IFREG | 0644;          /* console / other: size 0 */
    }

    if (copyout((void __user *)(uintptr_t)ustat, &st, sizeof st) < 0)
        return -EFAULT;
    return 0;
}

/* DDR-742: enumerate a directory one entry at a time. Resolves `path` against
 * the caller's root mount + fs cap (honoring per-process roots, DDR-739), reads
 * the index-th name via vfs_readdir, and copies it out NUL-terminated. Returns
 * the name length (>0), 0 when index is past the last entry (end), or -errno. */
static long sys_getdents(long upath, long index, long ubuf, long a4) {
    (void)a4;
    struct tcb *t = current_thread;
    if (t->root_mnt < 0)
        return -ENOENT;
    if (index < 0)
        return -EINVAL;
    char path[PATH_MAX];
    if (copyinstr(path, (const void __user *)(uintptr_t)upath, sizeof path, 0) < 0)
        return -EFAULT;
    char name[256];
    uint32_t sz = 0;
    if (vfs_readdir(t->fs_cap, t->root_mnt, path, (int)index, name, &sz) != 0)
        return 0;                              /* no entry at this index = end of dir */
    int len = 0;
    while (len < 255 && name[len]) len++;
    if (copyout((void __user *)(uintptr_t)ubuf, name, (size_t)len + 1) < 0)
        return -EFAULT;
    return len;
}

void sys_file_register(void) {
    syscall_register(SYS_OPEN,  sys_open);
    syscall_register(SYS_CLOSE, sys_close);
    syscall_register(SYS_FSTAT, sys_fstat);
    syscall_register(SYS_GETDENTS, sys_getdents);   /* DDR-742 */
}
