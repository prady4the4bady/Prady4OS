/* kernel/fs/vfs/vfs.c — VFS dispatch + mount table + capability gating. */
#include "vfs.h"
#include "blk.h"
#include "sched.h"      /* current_thread for the capability check + write budget */
#include "irq.h"        /* ADR-032: g_ticks for the token-bucket refill */
#include "errno.h"      /* DDR-888: distinct precondition codes from vfs_create */

static const struct vfs_fs_ops *g_fs[VFS_MAX_FS];
static unsigned                 g_nfs;

struct vfs_mount {
    const struct vfs_fs_ops *fs;
    struct blk_device       *bd;
    void                    *ctx;      /* FS-private per-mount context */
    int                      used;
    int                      busy;     /* DDR-SMP-3c-locks-3: per-mount sleep-mutex */
};
static struct vfs_mount g_mounts[VFS_MAX_MOUNTS];

/* DDR-SMP-3c-locks-3: serialize the FS driver's in-memory metadata (SFS
 * journal/B-tree, FAT cursor) across CPUs. A sleep-mutex, NOT a spinlock — the
 * FS op it guards descends into blk submit() which sched_block()s, and a
 * spinlock held across a block deadlocks spinners. Lock order is always
 * mount -> blk, never reversed. No IRQ touches `busy`, so no cli/sti. */
static void mnt_lock(struct vfs_mount *m) {
    while (__atomic_exchange_n(&m->busy, 1, __ATOMIC_ACQUIRE))
        yield();
}
static void mnt_unlock(struct vfs_mount *m) {
    __atomic_store_n(&m->busy, 0, __ATOMIC_RELEASE);
}

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

static int g_default_mnt = -1;          /* process root mount (5b, ADR-022) */
void vfs_set_default_mnt(int mnt) { g_default_mnt = mnt; }
int  vfs_default_mnt(void)        { return g_default_mnt; }

void vfs_register(const struct vfs_fs_ops *ops) {
    if (g_nfs < VFS_MAX_FS)
        g_fs[g_nfs++] = ops;
}

int vfs_mount(unsigned blk_index) {
    struct blk_device *bd = blk_get(blk_index);
    if (!bd)
        return -1;
    int id = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (!g_mounts[i].used) { id = i; break; }
    if (id < 0)
        return -1;                     /* mount table full */
    for (unsigned i = 0; i < g_nfs; i++) {
        void *ctx = 0;
        if (g_fs[i]->mount(bd, &ctx) == 0) {
            g_mounts[id].fs   = g_fs[i];
            g_mounts[id].bd   = bd;
            g_mounts[id].ctx  = ctx;
            g_mounts[id].used = 1;
            return id;
        }
    }
    return -1;                          /* no driver claimed the disk */
}

/* The kernel's string.h has no strcmp — comparing two NUL-terminated names is
 * the only string operation these paths need, so it is local rather than an
 * addition to a shared header for one caller. */
static int name_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

/* DDR-890 (item 40): mount a filesystem that has NO block device.
 *
 * vfs_mount() probes drivers against a disk; a virtual filesystem has nothing to
 * probe, so it is named explicitly. Selecting BY NAME rather than "the driver
 * that accepts NULL" matters: two virtual filesystems would both accept NULL and
 * the caller would silently get whichever registered first.
 */
int vfs_mount_virtual(const char *fsname) {
    int id = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (!g_mounts[i].used) { id = i; break; }
    if (id < 0)
        return -1;                     /* mount table full */
    for (unsigned i = 0; i < g_nfs; i++) {
        if (!g_fs[i]->name || !name_eq(g_fs[i]->name, fsname))
            continue;
        void *ctx = 0;
        if (g_fs[i]->mount(0, &ctx) != 0)
            return -1;                 /* named driver refused: do NOT fall back */
        g_mounts[id].fs   = g_fs[i];
        g_mounts[id].bd   = 0;
        g_mounts[id].ctx  = ctx;
        g_mounts[id].used = 1;
        return id;
    }
    return -1;                          /* no driver by that name */
}

static struct vfs_mount *mnt_get(int id) {
    if (id < 0 || id >= VFS_MAX_MOUNTS || !g_mounts[id].used)
        return 0;
    return &g_mounts[id];
}

const char *vfs_fs_name(int mnt) {
    struct vfs_mount *m = mnt_get(mnt);
    return m ? m->fs->name : 0;
}

static int cap_ok(cap_t cap, uint32_t right) {
    return cap_authorize(current_thread->caps, cap, RES_FILE, FS_RES_ID, right);
}

int vfs_open(cap_t cap, int mnt, const char *path, struct vfs_file *out) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m || !m->fs->open || !cap_ok(cap, CAP_FS_READ))
        return -1;
    if (!mnt_lock_live(m)) return -EIO;   /* DDR-954 */
    int r = m->fs->open(m->ctx, path, out);
    mnt_unlock(m);
    if (r == 0) out->mnt = mnt;
    return r;
}

int vfs_create(cap_t cap, int mnt, const char *path, struct vfs_file *out) {
    struct vfs_mount *m = mnt_get(mnt);
    /* DDR-888: split the precondition failure from the driver's own return.
     * Both used to be a bare -1, so `[sfs] churn FAIL op=create iter=0 rc=-1`
     * could not distinguish "this mount/capability was never usable" from "the
     * filesystem refused the create" — two unrelated defects. -1 also matches
     * none of DDR-884's candidates (-EEXIST=-17, -ENOSPC=-28, ADR-032 budget),
     * which is itself evidence that this is the precondition branch.
     * All 20 in-tree callers test ==0/!=0; none compares against -1. */
    if (!cap_ok(cap, CAP_FS_WRITE))
        return -EPERM;                 /* capability problem */
    if (!m || !m->fs->create)
        return -EINVAL;                /* mount problem: bad id, or no create op */
    if (!mnt_lock_live(m)) return -EIO;   /* DDR-954 */
    int r = m->fs->create(m->ctx, path, out);
    mnt_unlock(m);
    if (r == 0) out->mnt = mnt;
    return r;
}

int vfs_read(cap_t cap, const struct vfs_file *f, uint64_t off, void *buf, uint32_t len) {
    struct vfs_mount *m = mnt_get(f->mnt);
    if (!m || !m->fs->read || !cap_ok(cap, CAP_FS_READ))
        return -1;
    if (!mnt_lock_live(m)) return -EIO;   /* DDR-954 */
    int r = m->fs->read(m->ctx, f, off, buf, len);
    mnt_unlock(m);
    return r;
}

/* DDR-866 (item 20): set a file's length.
 *
 * Gated on CAP_FS_WRITE, not a metadata capability: truncate destroys file
 * content exactly as a write does, and a caller able to shrink a file to
 * zero without CAP_FS_WRITE would hold a delete primitive that bypasses the
 * write gate entirely.
 *
 * Deliberately NOT charged to the ADR-032 token bucket that vfs_write uses.
 * That bucket bounds write THROUGHPUT — caller bytes pushed through the FS —
 * and a truncate moves none. Charging it would let one length change exhaust
 * a process's budget, and leaving it uncharged is safe because truncate
 * cannot write attacker-chosen data. */
int vfs_truncate(cap_t cap, struct vfs_file *f, uint64_t len) {
    struct vfs_mount *m = mnt_get(f->mnt);
    if (!m || !m->fs->truncate || !cap_ok(cap, CAP_FS_WRITE))
        return -1;
    if (!mnt_lock_live(m)) return -EIO;   /* DDR-954 */
    int r = m->fs->truncate(m->ctx, f, len);
    mnt_unlock(m);
    return r;
}

int vfs_write(cap_t cap, struct vfs_file *f, uint64_t off, const void *buf, uint32_t len) {
    struct vfs_mount *m = mnt_get(f->mnt);
    if (!m || !m->fs->write || !cap_ok(cap, CAP_FS_WRITE))
        return -1;
    /* ADR-032: token-bucket write rate limit. Lazily refill the thread's bucket
     * from elapsed ticks (capped at the burst max), then require enough tokens.
     * Refill only tops up a below-cap balance and never reduces a higher one, so
     * a kernel self-test's `fs_write_budget = ~0ull` bypass is preserved. This
     * bounds the write RATE (anti-flood) without a lifetime cap. */
    uint64_t now = g_ticks;
    if (current_thread->fs_write_budget < FS_WRITE_BURST_MAX &&
        now > current_thread->fs_budget_tick) {
        uint64_t add = (now - current_thread->fs_budget_tick) * FS_WRITE_REFILL_PER_TICK;
        uint64_t bal = current_thread->fs_write_budget + add;
        current_thread->fs_write_budget =
            (bal > FS_WRITE_BURST_MAX) ? FS_WRITE_BURST_MAX : bal;
    }
    current_thread->fs_budget_tick = now;
    if (current_thread->fs_write_budget < len)
        return -1;
    if (!mnt_lock_live(m)) return -EIO;   /* DDR-954 */
    int r = m->fs->write(m->ctx, f, off, buf, len);
    mnt_unlock(m);
    if (r > 0)
        current_thread->fs_write_budget -= (uint64_t)r;
    return r;
}

int vfs_unlink(cap_t cap, int mnt, const char *path) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m || !m->fs->unlink || !cap_ok(cap, CAP_FS_WRITE))
        return -1;
    if (!mnt_lock_live(m)) return -EIO;   /* DDR-954 */
    int r = m->fs->unlink(m->ctx, path);
    mnt_unlock(m);
    return r;
}

int vfs_readdir(cap_t cap, int mnt, const char *path, int index, char *name, uint32_t *size) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m || !m->fs->readdir || !cap_ok(cap, CAP_FS_READ))
        return -1;
    if (!mnt_lock_live(m)) return -EIO;   /* DDR-954 */
    int r = m->fs->readdir(m->ctx, path, index, name, size);
    mnt_unlock(m);
    return r;
}

int vfs_unmount(int mnt) {
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
}

int vfs_txn_begin(cap_t cap, int mnt) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m || !m->fs->txn_begin || !cap_ok(cap, CAP_FS_WRITE))
        return -1;
    if (!mnt_lock_live(m)) return -EIO;   /* DDR-954 */
    int r = m->fs->txn_begin(m->ctx);
    mnt_unlock(m);
    return r;
}

int vfs_txn_commit(cap_t cap, int mnt) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m || !m->fs->txn_commit || !cap_ok(cap, CAP_FS_WRITE))
        return -1;
    if (!mnt_lock_live(m)) return -EIO;   /* DDR-954 */
    int r = m->fs->txn_commit(m->ctx);
    mnt_unlock(m);
    return r;
}

int vfs_txn_abort(cap_t cap, int mnt) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m || !m->fs->txn_abort || !cap_ok(cap, CAP_FS_WRITE))
        return -1;
    if (!mnt_lock_live(m)) return -EIO;   /* DDR-954 */
    int r = m->fs->txn_abort(m->ctx);
    mnt_unlock(m);
    return r;
}
