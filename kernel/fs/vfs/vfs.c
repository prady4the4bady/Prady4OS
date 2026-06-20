/* kernel/fs/vfs/vfs.c — VFS dispatch + mount table + capability gating. */
#include "vfs.h"
#include "blk.h"
#include "sched.h"      /* current_thread for the capability check + write budget */

static const struct vfs_fs_ops *g_fs[VFS_MAX_FS];
static unsigned                 g_nfs;

struct vfs_mount {
    const struct vfs_fs_ops *fs;
    struct blk_device       *bd;
    void                    *ctx;      /* FS-private per-mount context */
    int                      used;
};
static struct vfs_mount g_mounts[VFS_MAX_MOUNTS];

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
    int r = m->fs->open(m->ctx, path, out);
    if (r == 0) out->mnt = mnt;
    return r;
}

int vfs_create(cap_t cap, int mnt, const char *path, struct vfs_file *out) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m || !m->fs->create || !cap_ok(cap, CAP_FS_WRITE))
        return -1;
    int r = m->fs->create(m->ctx, path, out);
    if (r == 0) out->mnt = mnt;
    return r;
}

int vfs_read(cap_t cap, const struct vfs_file *f, uint64_t off, void *buf, uint32_t len) {
    struct vfs_mount *m = mnt_get(f->mnt);
    if (!m || !m->fs->read || !cap_ok(cap, CAP_FS_READ))
        return -1;
    return m->fs->read(m->ctx, f, off, buf, len);
}

int vfs_write(cap_t cap, struct vfs_file *f, uint64_t off, const void *buf, uint32_t len) {
    struct vfs_mount *m = mnt_get(f->mnt);
    if (!m || !m->fs->write || !cap_ok(cap, CAP_FS_WRITE))
        return -1;
    /* Per-process write budget: a buggy/hostile consumer cannot exhaust the
     * whole block device — it can only write what its thread was granted. */
    if (current_thread->fs_write_budget < len)
        return -1;
    int r = m->fs->write(m->ctx, f, off, buf, len);
    if (r > 0)
        current_thread->fs_write_budget -= (uint64_t)r;
    return r;
}

int vfs_unlink(cap_t cap, int mnt, const char *path) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m || !m->fs->unlink || !cap_ok(cap, CAP_FS_WRITE))
        return -1;
    return m->fs->unlink(m->ctx, path);
}

int vfs_readdir(cap_t cap, int mnt, const char *path, int index, char *name, uint32_t *size) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m || !m->fs->readdir || !cap_ok(cap, CAP_FS_READ))
        return -1;
    return m->fs->readdir(m->ctx, path, index, name, size);
}

int vfs_unmount(int mnt) {
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
}

int vfs_txn_begin(cap_t cap, int mnt) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m || !m->fs->txn_begin || !cap_ok(cap, CAP_FS_WRITE))
        return -1;
    return m->fs->txn_begin(m->ctx);
}

int vfs_txn_commit(cap_t cap, int mnt) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m || !m->fs->txn_commit || !cap_ok(cap, CAP_FS_WRITE))
        return -1;
    return m->fs->txn_commit(m->ctx);
}

int vfs_txn_abort(cap_t cap, int mnt) {
    struct vfs_mount *m = mnt_get(mnt);
    if (!m || !m->fs->txn_abort || !cap_ok(cap, CAP_FS_WRITE))
        return -1;
    return m->fs->txn_abort(m->ctx);
}
