#!/usr/bin/env python3
"""DDR-956 step 2b: vfs.h op slot + declaration, vfs.c vfs_rename.
Spacing matches the file's aligned column style, verified by grep."""
import io
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
def sub(s, old, new, tag):
    assert s.count(old) == 1, "%s: expected 1, got %d" % (tag, s.count(old))
    return s.replace(old, new, 1)

p = "kernel/fs/vfs/vfs.h"; s = rd(p)
assert "vfs_rename" not in s, "vfs.h already patched"
s = sub(s, "    int (*unlink) (void *ctx, const char *path);",
        "    int (*unlink) (void *ctx, const char *path);\n"
        "    /* DDR-956: optional; NULL-safe -- vfs_rename returns -ENOSYS. */\n"
        "    int (*rename) (void *ctx, const char *old_path, const char *new_path);",
        "vfs.h op")
s = sub(s, "int  vfs_unlink (cap_t cap, int mnt, const char *path);",
        "int  vfs_unlink (cap_t cap, int mnt, const char *path);\n"
        "/* DDR-956: rename within ONE mount. Every caller passes t->root_mnt and no\n"
        " * path->mount resolution exists, so -EXDEV is unreachable and absent. */\n"
        "int  vfs_rename (cap_t cap, int mnt, const char *old_path, const char *new_path);",
        "vfs.h decl")
wr(p, s); print("vfs.h ok")

p = "kernel/fs/vfs/vfs.c"; s = rd(p)
assert "vfs_rename" not in s, "vfs.c already patched"
old_unlink = """int vfs_unlink(cap_t cap, int mnt, const char *path) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m || !m->fs->unlink || !cap_ok(cap, CAP_FS_WRITE))
        return -1;
    if (!mnt_lock_live(m)) return -EIO;   /* DDR-954 */
    int r = m->fs->unlink(m->ctx, path);
    mnt_unlock(m);
    return r;
}"""
new_block = old_unlink + '''

/* DDR-956: rename, modelled exactly on vfs_unlink above -- same capability
 * gate, same mnt_lock_live() revalidation (DDR-954), unlock on every path.
 *
 * There is deliberately NO -EXDEV branch. Every syscall passes t->root_mnt
 * (sys_file.c) and no path->mount resolution exists anywhere in the tree, so a
 * caller cannot name a second mount; an EXDEV check would be unreachable. */
int vfs_rename(cap_t cap, int mnt, const char *old_path, const char *new_path) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m || !cap_ok(cap, CAP_FS_WRITE))
        return -1;
    if (!m->fs->rename)
        return -ENOSYS;
    if (!mnt_lock_live(m)) return -EIO;   /* DDR-954 */
    int r = m->fs->rename(m->ctx, old_path, new_path);
    mnt_unlock(m);
    return r;
}'''
s = sub(s, old_unlink, new_block, "vfs_rename")
wr(p, s); print("vfs.c ok")
