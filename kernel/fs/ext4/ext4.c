/* kernel/fs/ext4/ext4.c — ext4 read-only driver (Phase 4j).
 *
 * Supports: extent-mapped regular files (depth-0 extent tree, i.e. up to 4
 * inline extents), linear directory scan, and nested paths. Read-only.
 * Block-mapped inodes and multi-level extent index nodes are deferred.
 */
#include "ext4.h"
#include "vfs.h"
#include "blk.h"
#include "pmm.h"
#include "kheap.h"
#include "string.h"

#define EXT4_MAGIC          0xEF53u
#define EXT4_ROOT_INO       2u
#define EXT4_INCOMPAT_64BIT 0x80u
#define EXT4_EXTENTS_FL     0x80000u
#define EXT4_EXTENT_MAGIC   0xF30Au
#define EXT4_S_IFDIR        0x4000u
#define EXT4_S_IFREG        0x8000u

struct ext4_ctx {
    struct blk_device *bd;
    uint32_t block_size;
    uint32_t spb;                  /* sectors per block (block_size/512)         */
    uint32_t inode_size;
    uint32_t inodes_per_group;
    uint32_t desc_size;            /* group descriptor size (32 or 64)           */
    uint32_t gd_block;             /* first block of the group descriptor table  */
    int      is64;
    uint64_t blkbuf;               /* one block scratch (PMM page)               */
    uint64_t inobuf;               /* one block scratch for inode reads          */
};

static uint16_t rd16(const uint8_t *p, int o) { return p[o] | ((uint16_t)p[o + 1] << 8); }
static uint32_t rd32(const uint8_t *p, int o) {
    return p[o] | ((uint32_t)p[o + 1] << 8) | ((uint32_t)p[o + 2] << 16) | ((uint32_t)p[o + 3] << 24);
}

/* Read one ext4 block into `buf` (block_size bytes; buf is a PMM page). */
static void e4_read_block(struct ext4_ctx *c, uint64_t blk, void *buf) {
    c->bd->read(c->bd, blk * c->spb, buf, c->spb);
}

/* Load inode `ino` into `out` (out has room for inode_size bytes). 0 on success. */
static int e4_read_inode(struct ext4_ctx *c, uint32_t ino, uint8_t *out) {
    if (ino == 0)
        return -1;
    uint32_t group = (ino - 1) / c->inodes_per_group;
    uint32_t idx   = (ino - 1) % c->inodes_per_group;

    /* group descriptor -> inode table block */
    uint64_t desc_byte = (uint64_t)c->gd_block * c->block_size + (uint64_t)group * c->desc_size;
    uint64_t desc_blk  = desc_byte / c->block_size;
    uint32_t desc_off  = (uint32_t)(desc_byte % c->block_size);
    uint8_t *bb = (uint8_t *)(uintptr_t)c->blkbuf;
    e4_read_block(c, desc_blk, bb);
    uint64_t itable = rd32(bb, desc_off + 0x08);
    if (c->is64 && c->desc_size >= 64)
        itable |= (uint64_t)rd32(bb, desc_off + 0x28) << 32;

    /* inode within the table */
    uint64_t ino_byte = itable * c->block_size + (uint64_t)idx * c->inode_size;
    uint64_t ino_blk  = ino_byte / c->block_size;
    uint32_t ino_off  = (uint32_t)(ino_byte % c->block_size);
    uint8_t *ib = (uint8_t *)(uintptr_t)c->inobuf;
    e4_read_block(c, ino_blk, ib);
    /* an inode may straddle two blocks only if inode_size > block_size, which we
     * do not support; inode_size (256) <= block_size always here. */
    for (uint32_t i = 0; i < c->inode_size; i++)
        out[i] = ib[ino_off + i];
    return 0;
}

/* Map a file's logical block to a physical block via the depth-0 extent tree.
 * Returns the physical block, or 0 if unmapped/unsupported. */
static uint64_t e4_bmap(const uint8_t *inode, uint32_t lblk) {
    const uint8_t *eh = inode + 0x28;             /* i_block holds the extent header */
    if (rd16(eh, 0) != EXT4_EXTENT_MAGIC)
        return 0;                                  /* block-mapped: unsupported */
    if (rd16(eh, 6) != 0)
        return 0;                                  /* depth > 0: unsupported */
    uint16_t n = rd16(eh, 2);
    for (uint16_t i = 0; i < n && i < 4; i++) {
        const uint8_t *ee = eh + 12 + (uint32_t)i * 12;
        uint32_t ee_block = rd32(ee, 0);
        uint16_t ee_len = rd16(ee, 4);
        if (ee_len > 32768) ee_len -= 32768;       /* uninitialized extent */
        uint64_t start = (uint64_t)rd32(ee, 8) | ((uint64_t)rd16(ee, 6) << 32);
        if (lblk >= ee_block && lblk < ee_block + ee_len)
            return start + (lblk - ee_block);
    }
    return 0;
}

/* Find `name` (len) in the directory inode; returns its inode number or 0. */
static uint32_t e4_dir_lookup(struct ext4_ctx *c, const uint8_t *dir_inode,
                              const char *name, int len) {
    uint64_t dsize = rd32(dir_inode, 4) | ((uint64_t)rd32(dir_inode, 108) << 32);
    uint64_t nblocks = (dsize + c->block_size - 1) / c->block_size;
    uint8_t *bb = (uint8_t *)(uintptr_t)c->blkbuf;
    for (uint64_t lb = 0; lb < nblocks; lb++) {
        uint64_t pb = e4_bmap(dir_inode, (uint32_t)lb);
        if (!pb) continue;
        e4_read_block(c, pb, bb);
        uint32_t off = 0;
        while (off + 8 <= c->block_size) {
            uint32_t ino = rd32(bb, off);
            uint16_t rec = rd16(bb, off + 4);
            uint8_t  nl  = bb[off + 6];
            if (rec < 8) break;
            if (ino != 0 && nl == len) {
                int match = 1;
                for (int i = 0; i < len; i++)
                    if ((char)bb[off + 8 + i] != name[i]) { match = 0; break; }
                if (match)
                    return ino;
            }
            off += rec;
        }
    }
    return 0;
}

/* Resolve an absolute path to an inode number (0 on failure); fills *is_dir. */
static uint32_t e4_resolve(struct ext4_ctx *c, const char *path, uint8_t *inode_out, int *is_dir) {
    uint32_t ino = EXT4_ROOT_INO;
    if (e4_read_inode(c, ino, inode_out) != 0)
        return 0;
    while (*path == '/') path++;
    while (*path) {
        int len = 0;
        while (path[len] && path[len] != '/') len++;
        if (len == 0) break;
        if ((rd16(inode_out, 0) & 0xF000) != EXT4_S_IFDIR)
            return 0;
        uint32_t child = e4_dir_lookup(c, inode_out, path, len);
        if (!child)
            return 0;
        if (e4_read_inode(c, child, inode_out) != 0)
            return 0;
        ino = child;
        path += len;
        while (*path == '/') path++;
    }
    if (is_dir)
        *is_dir = (rd16(inode_out, 0) & 0xF000) == EXT4_S_IFDIR;
    return ino;
}

/* ---- VFS operations (read-only) ------------------------------------------ */

static int ext4_open(void *ctx, const char *path, struct vfs_file *out) {
    struct ext4_ctx *c = (struct ext4_ctx *)ctx;
    uint64_t inp = pmm_alloc_page();
    if (!inp) return -1;
    uint8_t *inode = (uint8_t *)(uintptr_t)inp;
    int is_dir = 0;
    uint32_t ino = e4_resolve(c, path, inode, &is_dir);
    int rc = -1;
    if (ino && !is_dir) {
        out->size = rd32(inode, 4) | ((uint64_t)rd32(inode, 108) << 32);
        out->cookie = ino;
        out->dirent_clus = 0;
        out->dirent_off = 0;
        rc = 0;
    }
    pmm_free_page(inp);
    return rc;
}

static int ext4_read(void *ctx, const struct vfs_file *f, uint64_t off, void *buf, uint32_t len) {
    struct ext4_ctx *c = (struct ext4_ctx *)ctx;
    uint64_t inp = pmm_alloc_page();
    if (!inp) return -1;
    uint8_t *inode = (uint8_t *)(uintptr_t)inp;
    if (e4_read_inode(c, (uint32_t)f->cookie, inode) != 0) { pmm_free_page(inp); return -1; }
    uint64_t fsize = rd32(inode, 4) | ((uint64_t)rd32(inode, 108) << 32);

    uint64_t bbp = pmm_alloc_page();
    if (!bbp) { pmm_free_page(inp); return -1; }
    uint8_t *bb = (uint8_t *)(uintptr_t)bbp;
    uint8_t *outb = (uint8_t *)buf;
    uint32_t copied = 0;
    while (copied < len && off + copied < fsize) {
        uint64_t fo = off + copied;
        uint32_t lblk = (uint32_t)(fo / c->block_size);
        uint32_t boff = (uint32_t)(fo % c->block_size);
        uint64_t pb = e4_bmap(inode, lblk);
        if (!pb) break;
        e4_read_block(c, pb, bb);
        while (boff < c->block_size && copied < len && off + copied < fsize)
            outb[copied++] = bb[boff++];
    }
    pmm_free_page(bbp);
    pmm_free_page(inp);
    return (int)copied;
}

static int ext4_readdir(void *ctx, const char *path, int index, char *name, uint32_t *size) {
    struct ext4_ctx *c = (struct ext4_ctx *)ctx;
    uint64_t inp = pmm_alloc_page();
    if (!inp) return -1;
    uint8_t *inode = (uint8_t *)(uintptr_t)inp;
    int is_dir = 0;
    uint32_t ino = e4_resolve(c, path, inode, &is_dir);
    if (!ino || !is_dir) { pmm_free_page(inp); return -1; }

    uint64_t dsize = rd32(inode, 4) | ((uint64_t)rd32(inode, 108) << 32);
    uint64_t nblocks = (dsize + c->block_size - 1) / c->block_size;
    uint64_t bbp = pmm_alloc_page();
    if (!bbp) { pmm_free_page(inp); return -1; }
    uint8_t *bb = (uint8_t *)(uintptr_t)bbp;
    int count = 0, rc = -1;
    for (uint64_t lb = 0; lb < nblocks && rc != 0; lb++) {
        uint64_t pb = e4_bmap(inode, (uint32_t)lb);
        if (!pb) continue;
        e4_read_block(c, pb, bb);
        uint32_t off = 0;
        while (off + 8 <= c->block_size) {
            uint32_t dino = rd32(bb, off);
            uint16_t rec = rd16(bb, off + 4);
            uint8_t  nl  = bb[off + 6];
            if (rec < 8) break;
            if (dino != 0 && nl > 0 && nl <= 15) {     /* skip ./.. handled by name */
                if (count == index) {
                    for (int i = 0; i < nl; i++) name[i] = (char)bb[off + 8 + i];
                    name[nl] = 0;
                    if (size) *size = 0;
                    rc = 0;
                    break;
                }
                count++;
            }
            off += rec;
        }
    }
    pmm_free_page(bbp);
    pmm_free_page(inp);
    return rc;
}

/* ---- mount / register ---------------------------------------------------- */

static int ext4_mount(struct blk_device *bd, void **ctx) {
    if (!bd)
        return -1;
    uint64_t sp = pmm_alloc_page();
    if (!sp)
        return -1;
    uint8_t *sb = (uint8_t *)(uintptr_t)sp;
    /* superblock is 1024 bytes at byte offset 1024 -> LBA 2 (512-byte sectors) */
    bd->read(bd, 2, sb, 2);
    if (rd16(sb, 0x38) != EXT4_MAGIC) { pmm_free_page(sp); return -1; }

    struct ext4_ctx *c = (struct ext4_ctx *)kmalloc(sizeof(struct ext4_ctx));
    if (!c) { pmm_free_page(sp); return -1; }
    c->bd = bd;
    uint32_t log_bs = rd32(sb, 0x18);
    c->block_size = 1024u << log_bs;
    c->spb = c->block_size / 512u;
    c->inodes_per_group = rd32(sb, 0x28);
    c->inode_size = rd16(sb, 0x58);
    if (c->inode_size == 0) c->inode_size = 128;
    uint32_t feat_incompat = rd32(sb, 0x60);
    c->is64 = (feat_incompat & EXT4_INCOMPAT_64BIT) ? 1 : 0;
    c->desc_size = c->is64 ? rd16(sb, 0xFE) : 32;
    if (c->desc_size == 0) c->desc_size = 32;
    c->gd_block = (c->block_size == 1024) ? 2 : 1;

    pmm_free_page(sp);

    /* The scratch buffers are single 4 KiB pages, so only block sizes up to
     * 4096 (and a sane inode size) are supported. */
    if (c->block_size > 4096u || c->inode_size > c->block_size) {
        kfree(c);
        return -1;
    }
    c->blkbuf = pmm_alloc_page();
    c->inobuf = pmm_alloc_page();
    if (!c->blkbuf || !c->inobuf) {
        if (c->blkbuf) pmm_free_page(c->blkbuf);
        if (c->inobuf) pmm_free_page(c->inobuf);
        kfree(c);
        return -1;
    }
    *ctx = c;
    return 0;
}

static void ext4_umount(void *ctx) {
    struct ext4_ctx *c = (struct ext4_ctx *)ctx;
    if (c->blkbuf) pmm_free_page(c->blkbuf);
    if (c->inobuf) pmm_free_page(c->inobuf);
    kfree(c);
}

static const struct vfs_fs_ops ext4_ops = {
    .name    = "ext4",
    .mount   = ext4_mount,
    .open    = ext4_open,
    .read    = ext4_read,
    .readdir = ext4_readdir,
    .umount  = ext4_umount,
    /* create/write/unlink/txn = NULL: read-only */
};

void ext4_register(void) {
    vfs_register(&ext4_ops);
}
