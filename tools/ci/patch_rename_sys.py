#!/usr/bin/env python3
"""DDR-956 step 2c: SYS_RENAME=95 + handler, modelled on sys_unlink."""
import io
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
def sub(s, old, new, tag):
    assert s.count(old) == 1, "%s: expected 1, got %d" % (tag, s.count(old))
    return s.replace(old, new, 1)

p = "kernel/syscall/syscall.h"; s = rd(p)
assert "SYS_RENAME" not in s, "already defined"
s = sub(s, "#define SYS_FTRUNCATE      94",
        "#define SYS_RENAME         95  /* (oldpath, newpath) -> 0 | -ENOENT|-EPERM|-EIO */\n"
        "#define SYS_FTRUNCATE      94", "syscall.h")
wr(p, s); print("SYS_RENAME=95 defined")

p = "kernel/syscall/sys_file.c"; s = rd(p)
assert "sys_rename" not in s, "already present"
old_unlink = """static long sys_unlink(long upath, long a2, long a3, long a4, long a5, long a6) {"""
i = s.index(old_unlink)
j = s.index("\n}\n", i) + len("\n}\n")
handler = '''
/* DDR-956: rename(old, new) within the caller's root mount.
 *
 * Modelled on sys_unlink directly above: same t->root_mnt (there is no
 * path->mount resolution in this kernel), same copyinstr of every user pointer,
 * same capability handed to the VFS. Two PATH_MAX buffers is 512 bytes of
 * syscall stack, matching what sys_exec already spends.
 *
 * The backend's generic -1 collapses to -ENOENT exactly as sys_unlink does, but
 * a distinct negative errno from the VFS (-ENOSYS when a filesystem has no
 * rename op, -EIO from the DDR-954 mount revalidation) is passed through rather
 * than flattened -- those say something different from "no such file". */
static long sys_rename(long uold, long unew, long a3, long a4, long a5, long a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_thread;
    if (t->root_mnt < 0)
        return -ENOENT;
    char oldp[PATH_MAX], newp[PATH_MAX];
    if (copyinstr(oldp, (const void __user *)(uintptr_t)uold, sizeof oldp, 0) < 0)
        return -EFAULT;
    if (copyinstr(newp, (const void __user *)(uintptr_t)unew, sizeof newp, 0) < 0)
        return -EFAULT;
    int r = vfs_rename(t->fs_cap, t->root_mnt, oldp, newp);
    if (r == 0)
        return 0;
    return (r < -1) ? r : -ENOENT;
}
'''
s = s[:j] + handler + s[j:]
s = sub(s, "    syscall_register(SYS_FTRUNCATE, sys_ftruncate); /* DDR-866 */",
        "    syscall_register(SYS_FTRUNCATE, sys_ftruncate); /* DDR-866 */\n"
        "    syscall_register(SYS_RENAME,   sys_rename);     /* DDR-956 */", "register")
wr(p, s); print("sys_rename added + registered")
