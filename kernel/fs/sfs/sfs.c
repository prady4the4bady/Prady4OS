/* kernel/fs/sfs/sfs.c — SOVEREIGN Filesystem (SFS), slice 1.
 *
 * Bring-up scope (ADR-018): in-kernel format, mount, and an empty-root readdir.
 * The copy-on-write B+ tree insert/lookup, file extents, journal, snapshots,
 * and LZ4 are later slices — create/open/read/write/unlink are still stubs.
 * Block size is 4 KiB (8 × 512-byte sectors); see sfs.h for the on-disk layout.
 */
#include "sfs.h"
#include "vfs.h"
#include "blk.h"
#include "pmm.h"
#include "kheap.h"
#include "console.h"
#include "string.h"

struct sfs_ctx {
    struct blk_device *bd;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t root_btree;
    uint64_t next_free;
    uint64_t generation;
    uint64_t blkbuf;               /* one 4 KiB scratch block (PMM page) */
};

/* Read/write one 4 KiB SFS block (= 8 sectors). */
static uint8_t *sfs_read_block(struct sfs_ctx *c, uint64_t blk) {
    c->bd->read(c->bd, blk * SFS_SECTORS_PER_BLOCK,
                (void *)(uintptr_t)c->blkbuf, SFS_SECTORS_PER_BLOCK);
    return (uint8_t *)(uintptr_t)c->blkbuf;
}

int sfs_format(struct blk_device *bd) {
    if (!bd || bd->capacity_sectors < SFS_SECTORS_PER_BLOCK * 2)
        return -1;
    uint64_t page = pmm_alloc_page();
    if (!page)
        return -1;
    uint8_t *b = (uint8_t *)(uintptr_t)page;
    uint64_t total = bd->capacity_sectors / SFS_SECTORS_PER_BLOCK;

    /* superblock at block 0 */
    memset(b, 0, SFS_BLOCK_SIZE);
    struct sfs_superblock *sb = (struct sfs_superblock *)b;
    sb->magic           = SFS_MAGIC;
    sb->version         = SFS_VERSION;
    sb->block_size      = SFS_BLOCK_SIZE;
    sb->total_blocks    = total;
    sb->root_btree      = 1;
    sb->snapshot_root   = 0;
    sb->free_extent_tree = 0;
    sb->txn_log_start   = 0;
    sb->txn_log_blocks  = 0;
    sb->generation      = 1;
    sb->next_free_block = 2;            /* blocks 0,1 used; allocate from 2 up */
    sb->features        = 0;
    sb->checksum        = 0;
    bd->write(bd, 0, b, SFS_SECTORS_PER_BLOCK);

    /* empty root B+ tree leaf at block 1 */
    memset(b, 0, SFS_BLOCK_SIZE);
    struct sfs_btree_node *root = (struct sfs_btree_node *)b;
    root->flags      = SFS_NODE_LEAF;
    root->nkeys      = 0;
    root->next_leaf  = 0;
    root->generation = 1;
    bd->write(bd, 1 * SFS_SECTORS_PER_BLOCK, b, SFS_SECTORS_PER_BLOCK);

    pmm_free_page(page);
    return 0;
}

/* Probe: claim the disk only if block 0 holds a valid SFS superblock. */
static int sfs_mount(struct blk_device *bd, void **ctx) {
    if (!bd)
        return -1;
    uint64_t page = pmm_alloc_page();
    if (!page)
        return -1;
    bd->read(bd, 0, (void *)(uintptr_t)page, SFS_SECTORS_PER_BLOCK);
    struct sfs_superblock *sb = (struct sfs_superblock *)(uintptr_t)page;
    if (sb->magic != SFS_MAGIC || sb->block_size != SFS_BLOCK_SIZE) {
        pmm_free_page(page);
        return -1;                     /* not an SFS volume */
    }
    struct sfs_ctx *c = (struct sfs_ctx *)kmalloc(sizeof(struct sfs_ctx));
    if (!c) {
        pmm_free_page(page);
        return -1;
    }
    c->bd          = bd;
    c->block_size  = sb->block_size;
    c->total_blocks = sb->total_blocks;
    c->root_btree  = sb->root_btree;
    c->next_free   = sb->next_free_block;
    c->generation  = sb->generation;
    c->blkbuf      = pmm_alloc_page();
    pmm_free_page(page);
    if (!c->blkbuf) {
        kfree(c);
        return -1;
    }
    *ctx = c;
    return 0;
}

/* List the root directory. Slice 1: the root B+ tree leaf is always empty, so
 * this reports no entries (returns -1 at index 0). */
static int sfs_readdir(void *ctx, const char *path, int index, char *name, uint32_t *size) {
    struct sfs_ctx *c = (struct sfs_ctx *)ctx;
    (void)path; (void)name; (void)size;
    uint8_t *b = sfs_read_block(c, c->root_btree);
    struct sfs_btree_node *node = (struct sfs_btree_node *)b;
    if (index < 0 || index >= (int)node->nkeys)
        return -1;                     /* empty directory */
    return -1;                         /* entry decoding lands in slice 2 */
}

/* Data ops are unimplemented until slices 2-3. */
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
