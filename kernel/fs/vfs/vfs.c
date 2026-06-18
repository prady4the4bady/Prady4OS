/* kernel/fs/vfs/vfs.c — VFS dispatch + capability gating. */
#include "vfs.h"
#include "blk.h"
#include "sched.h"      /* current_thread for the capability check */

#define VFS_MAX_FS 4

static const struct vfs_fs_ops *g_fs[VFS_MAX_FS];
static unsigned                 g_nfs;
static const struct vfs_fs_ops *g_mounted;

void vfs_register(const struct vfs_fs_ops *ops) {
    if (g_nfs < VFS_MAX_FS)
        g_fs[g_nfs++] = ops;
}

int vfs_mount(unsigned blk_index) {
    struct blk_device *bd = blk_get(blk_index);
    if (!bd)
        return -1;
    for (unsigned i = 0; i < g_nfs; i++) {
        if (g_fs[i]->mount(bd) == 0) {
            g_mounted = g_fs[i];
            return 0;
        }
    }
    return -1;
}

const char *vfs_fs_name(void) {
    return g_mounted ? g_mounted->name : 0;
}

static int cap_ok(cap_t cap, uint32_t right) {
    return cap_authorize(current_thread->caps, cap, RES_FILE, FS_RES_ID, right);
}

int vfs_open(cap_t cap, const char *path, struct vfs_file *out) {
    if (!g_mounted || !cap_ok(cap, CAP_FS_READ))
        return -1;
    return g_mounted->open(path, out);
}

int vfs_read(cap_t cap, const struct vfs_file *f, uint64_t off, void *buf, uint32_t len) {
    if (!g_mounted || !cap_ok(cap, CAP_FS_READ))
        return -1;
    return g_mounted->read(f, off, buf, len);
}

int vfs_readdir(cap_t cap, int index, char *name, uint32_t *size) {
    if (!g_mounted || !cap_ok(cap, CAP_FS_READ))
        return -1;
    return g_mounted->readdir(index, name, size);
}
