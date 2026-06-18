/* kernel/fs/sfs/sfs.c — SOVEREIGN Filesystem (SFS) driver skeleton.
 *
 * Phase 4 scaffold: every operation is a stub that reports "not supported".
 * The mount probe reads block 0 and only claims a volume whose superblock
 * carries SFS_MAGIC — none exist yet, so SFS never mounts and FAT32 stays the
 * only live filesystem. The real copy-on-write B+ tree engine (versioning,
 * atomic transactions, LZ4) lands in a later slice; see sfs.h for the on-disk
 * layout this will implement.
 */
#include "sfs.h"
#include "vfs.h"
#include "blk.h"

/* All data ops are unimplemented for now. */
static int sfs_open   (void *ctx, const char *path, struct vfs_file *out) {
    (void)ctx; (void)path; (void)out; return -1;
}
static int sfs_create (void *ctx, const char *path, struct vfs_file *out) {
    (void)ctx; (void)path; (void)out; return -1;
}
static int sfs_read   (void *ctx, const struct vfs_file *f, uint64_t off, void *buf, uint32_t len) {
    (void)ctx; (void)f; (void)off; (void)buf; (void)len; return -1;
}
static int sfs_write  (void *ctx, struct vfs_file *f, uint64_t off, const void *buf, uint32_t len) {
    (void)ctx; (void)f; (void)off; (void)buf; (void)len; return -1;
}
static int sfs_unlink (void *ctx, const char *path) {
    (void)ctx; (void)path; return -1;
}
static int sfs_readdir(void *ctx, const char *path, int index, char *name, uint32_t *size) {
    (void)ctx; (void)path; (void)index; (void)name; (void)size; return -1;
}

/* Probe: claim the volume only if block 0 holds an SFS superblock. No SFS
 * volumes exist yet, so this always declines — leaving FAT32 to claim disks. */
static int sfs_mount(struct blk_device *bd, void **ctx) {
    (void)ctx;
    if (!bd)
        return -1;
    /* A real implementation would read block 0 and validate the superblock:
     *   struct sfs_superblock sb; bd->read(...); if (sb.magic == SFS_MAGIC) ...
     * Until the engine exists we recognise nothing. */
    return -1;
}

static const struct vfs_fs_ops sfs_ops = {
    .name    = "sfs",
    .mount   = sfs_mount,
    .open    = sfs_open,
    .create  = sfs_create,
    .read    = sfs_read,
    .write   = sfs_write,
    .unlink  = sfs_unlink,
    .readdir = sfs_readdir,
};

void sfs_register(void) {
    vfs_register(&sfs_ops);
}
