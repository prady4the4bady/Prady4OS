#!/usr/bin/env python3
"""DDR-956 step 2a/2b: sfs_rename + vfs_rename + fs_ops wiring."""
import io
def rd(p): return io.open(p, encoding="utf-8", newline="").read()
def wr(p, s): io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
def sub(s, old, new, tag):
    assert s.count(old) == 1, "%s: expected 1, got %d" % (tag, s.count(old))
    return s.replace(old, new, 1)

# ---- sfs.c: sfs_rename, inserted after sfs_unlink -------------------------
p = "kernel/fs/sfs/sfs.c"; s = rd(p)
assert "sfs_rename" not in s, "sfs_rename already present"
anchor = ("    if (bt_insert(c, &s))\n"
          "        return -1;\n"
          "    sfs_commit(c);\n"
          "    return 0;\n"
          "}\n")
new_fn = anchor + '''
/* DDR-956: atomic rename. Same contract as sfs_unlink -- the VFS layer already
 * holds the per-mount sleep-mutex, so this function must NOT take it (mnt_lock
 * is not recursive; re-acquiring on this thread would spin against itself).
 *
 * Atomicity: BOTH key changes go into ONE journal transaction. bt_insert stages
 * the new name->inode entry and the old name's tombstone, and a single
 * sfs_commit flushes them together, so a crash replays both or neither. The
 * inode number is REUSED, never copied -- a copy-then-unlink shape would leave
 * the file duplicated in the window between the halves, and a crash there
 * leaves stale data behind. */
static int sfs_rename(void *ctx, const char *old_path, const char *new_path) {
    struct sfs_ctx *c = (struct sfs_ctx *)ctx;
    uint64_t oparent, nparent;
    const char *oname, *nname;
    int olen, nlen;

    if (sfs_walk(c, old_path, 0, &oparent, &oname, &olen) != 0)
        return -1;                       /* missing source or missing parent */
    if (sfs_walk(c, new_path, 0, &nparent, &nname, &nlen) != 0)
        return -1;                       /* destination parent must exist */

    uint64_t ino;
    if (sfs_do_lookup(c, oparent, oname, olen, &ino) != 0)
        return -1;                       /* absent or already a tombstone */

    uint64_t okey = (oparent << 32) | (uint64_t)sfs_name_hash32(oname, olen);
    uint64_t nkey = (nparent << 32) | (uint64_t)sfs_name_hash32(nname, nlen);
    if (okey == nkey)
        return 0;                        /* same entry: POSIX no-op */

    /* A directory source may only move onto an absent or empty destination,
     * mirroring the emptiness rule sfs_unlink applies before removing one. */
    uint64_t dst_ino;
    if (sfs_do_lookup(c, nparent, nname, nlen, &dst_ino) == 0 &&
        sfs_is_dir(c, dst_ino)) {
        char probe[256]; int cnt = 0;
        if (sfs_dir_walk(c, c->root_btree, dst_ino, 0, &cnt, probe) == 1)
            return -1;                   /* destination directory not empty */
    }

    struct sfs_leaf_slot s;

    /* 1: new name -> the SAME inode (bt_insert replaces any existing entry, so
     *    an occupied destination is overwritten inside this transaction). */
    memset(&s, 0, sizeof s);
    s.key = nkey;
    s.v.dir.inode_num = ino;
    s.v.dir.name_len = (uint8_t)nlen;
    for (int i = 0; i < nlen; i++) s.v.dir.name[i] = nname[i];
    if (bt_insert(c, &s))
        return -1;

    /* 2: old name -> tombstone, exactly as sfs_unlink writes it. */
    memset(&s, 0, sizeof s);
    s.key = okey;
    s.v.dir.inode_num = 0;
    s.v.dir.name_len = 0;
    if (bt_insert(c, &s))
        return -1;

    sfs_commit(c);                       /* ONE commit covers both changes */
    return 0;
}
'''
s = sub(s, anchor, new_fn, "sfs_rename")
s = sub(s, "    .unlink     = sfs_unlink,", "    .unlink     = sfs_unlink,\n    .rename     = sfs_rename,        /* DDR-956 */", "sfs vtable")
wr(p, s); print("sfs_rename added + vtable wired")

# ---- vfs.h: op slot + declaration -----------------------------------------
p = "kernel/fs/vfs/vfs.h"; s = rd(p)
s = sub(s, "    int (*unlink)(void *ctx, const char *path);",
        "    int (*unlink)(void *ctx, const char *path);\n"
        "    /* DDR-956: optional. NULL-safe -- vfs_rename returns -ENOSYS. */\n"
        "    int (*rename)(void *ctx, const char *old_path, const char *new_path);",
        "vfs.h op")
s = sub(s, "int  vfs_unlink(cap_t cap, int mnt, const char *path);",
        "int  vfs_unlink(cap_t cap, int mnt, const char *path);\n"
        "/* DDR-956: rename within ONE mount. No cross-mount form exists -- every\n"
        " * caller passes t->root_mnt, so -EXDEV is unreachable and is not implemented. */\n"
        "int  vfs_rename(cap_t cap, int mnt, const char *old_path, const char *new_path);",
        "vfs.h decl")
wr(p, s); print("vfs.h updated")

# ---- vfs.c: vfs_rename, modelled exactly on vfs_unlink --------------------
p = "kernel/fs/vfs/vfs.c"; s = rd(p)
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

/* DDR-956: rename, modelled exactly on vfs_unlink above -- same capability,
 * same mnt_lock_live() revalidation (DDR-954), same unlock on every path.
 *
 * There is deliberately no -EXDEV branch. Every syscall passes t->root_mnt
 * (sys_file.c), and no path->mount resolution exists anywhere in the tree, so a
 * caller cannot name a second mount. An EXDEV check would be unreachable code. */
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
wr(p, s); print("vfs_rename added")
