#!/usr/bin/env python3
"""DDR-954 root-fix: couple the FS context's lifetime to the mount lock.

Order matters: the op sites are rewritten FIRST, because the helper inserted
afterwards itself contains a `mnt_lock(m);` call and would otherwise be counted
and rewritten as if it were an op site (it was -- the count came out 11).
"""
import io
p = "kernel/fs/vfs/vfs.c"
s = io.open(p, encoding="utf-8", newline="").read()

# 1) Guard every op site FIRST. All ten are inside int-returning functions.
n = s.count("    mnt_lock(m);\n")
assert n == 10, "expected 10 op lock sites, found %d" % n
s = s.replace("    mnt_lock(m);\n",
              "    if (!mnt_lock_live(m)) return -EIO;   /* DDR-954 */\n")

# 2) Now add the helper (its own mnt_lock call is safe from the pass above).
old_unlock = """static void mnt_unlock(struct vfs_mount *m) {
    __atomic_store_n(&m->busy, 0, __ATOMIC_RELEASE);
}
"""
assert s.count(old_unlock) == 1, "mnt_unlock"
new_unlock = old_unlock + """
/* DDR-954: take the mount lock and prove the mount is STILL LIVE.
 *
 * Acquiring the lock is necessary but NOT sufficient. mnt_get() runs before the
 * lock, so a thread can resolve a live mount, block on `busy` behind an
 * in-flight op, and be handed the lock only after vfs_unmount has freed the
 * context underneath it. It would then operate on freed memory with the lock
 * correctly held. Re-checking used/ctx AFTER acquisition closes that window:
 * vfs_unmount clears both while holding the same lock, so any waiter that wakes
 * afterwards observes the cleared state and bails with -EIO instead of running
 * on a dead context.
 *
 * Returns 1 with the lock HELD, or 0 with the lock RELEASED. */
static int mnt_lock_live(struct vfs_mount *m) {
    mnt_lock(m);
    if (!m->used || !m->ctx) {
        mnt_unlock(m);
        return 0;
    }
    return 1;
}
"""
s = s.replace(old_unlock, new_unlock, 1)

# 3) vfs_unmount is the ONLY writer of ctx that never took the lock.
old_um = """int vfs_unmount(int mnt) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m)
        return -1;
    if (m->fs->umount)
        m->fs->umount(m->ctx);
    g_mounts[mnt].used = 0;
    g_mounts[mnt].ctx  = 0;
    g_mounts[mnt].fs   = 0;
    g_mounts[mnt].bd   = 0;
    return 0;
}"""
assert s.count(old_um) == 1, "vfs_unmount"
new_um = """int vfs_unmount(int mnt) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m)
        return -1;
    /* DDR-954 root-fix. Every other entry point serialises on this lock before
     * touching m->ctx; unmount did not, so it could kfree the context while a
     * ring-3 thread was executing inside sys_unlink with that same pointer
     * already loaded. Captured as: umount ctx=0x07C3C000 immediately followed
     * by a #PF at alloc_run with RDI=RSI=0x07C3C000 and RDX holding the
     * KHEAP_DEBUG free poison.
     *
     * Taking the lock here makes the free wait for the in-flight op to finish.
     * Clearing used/ctx BEFORE the release makes every thread queued behind us
     * fail its mnt_lock_live() re-check rather than run on freed memory. */
    mnt_lock(m);
    if (m->fs && m->fs->umount)
        m->fs->umount(m->ctx);
    m->ctx  = 0;
    m->used = 0;
    m->fs   = 0;
    m->bd   = 0;
    mnt_unlock(m);
    return 0;
}"""
s = s.replace(old_um, new_um, 1)

io.open(p, "w", encoding="utf-8", newline=chr(10)).write(s)
print("ddr954: %d op sites guarded, helper added, vfs_unmount serialised" % n)
