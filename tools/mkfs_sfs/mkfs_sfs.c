/* tools/mkfs_sfs/mkfs_sfs.c — host-side SFS image writer (DDR-767).
 *
 * Produces a byte-exact SOVEREIGN FS image the kernel mounts and reads. Reuses
 * the kernel's own on-disk structs + FNV-1a name hash by #including sfs.h, so the
 * layout can never drift from the kernel reader. Formats blocks 0-3 identically
 * to kernel sfs_format(), then optionally provisions root-level files (each a few
 * inline extents) into the single root B+tree leaf.
 *
 *   mkfs.sfs <image> [--blocks N] [--file NAME=hostpath] ...
 *
 * Little-endian host (x86_64) matches the kernel target; no byte-swapping.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "sfs.h"   /* struct sfs_superblock/node/inode/dirent, sfs_name_hash32 */

#define DEFAULT_BLOCKS 4096u             /* 16 MiB */
#define MAX_FILE_BYTES (4u * SFS_BLOCK_SIZE)   /* 4 inline extents * 4 KiB = 16 KiB */

static int          g_fd;
static uint64_t     g_next_free = 4;     /* blocks 0-3 are reserved */
static uint64_t     g_next_inode = SFS_ROOT_INODE + 1;
static struct sfs_node g_root_leaf;      /* block 1, built in memory */

static void die(const char *msg) {
    fprintf(stderr, "mkfs.sfs: %s: %s\n", msg, errno ? strerror(errno) : "error");
    exit(1);
}

static void wr_block(uint64_t idx, const void *buf) {
    if (pwrite(g_fd, buf, SFS_BLOCK_SIZE, (off_t)(idx * SFS_BLOCK_SIZE))
        != (ssize_t)SFS_BLOCK_SIZE)
        die("pwrite");
}

/* Append a slot to the in-memory root leaf. */
static void leaf_add(uint64_t key, const void *val, size_t vlen) {
    if (g_root_leaf.nkeys >= SFS_LEAF_MAX) {
        fprintf(stderr, "mkfs.sfs: root leaf full (max %u slots) — too many files\n",
                (unsigned)SFS_LEAF_MAX);
        exit(1);
    }
    struct sfs_leaf_slot *s = &g_root_leaf.u.leaf[g_root_leaf.nkeys++];
    memset(s, 0, sizeof *s);
    s->key = key;
    memcpy(&s->v, val, vlen);
}

static void add_root_file(const char *name, const uint8_t *data, uint32_t len) {
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > 255) { fprintf(stderr, "mkfs.sfs: bad name '%s'\n", name); exit(1); }
    if (len > MAX_FILE_BYTES) {
        fprintf(stderr, "mkfs.sfs: '%s' is %u B > %u B cap (this slice)\n",
                name, len, MAX_FILE_BYTES);
        exit(1);
    }

    uint64_t ino        = g_next_inode++;
    uint64_t inode_blk  = g_next_free++;
    uint32_t nblocks    = len ? (len + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE : 0;
    uint64_t data_start = g_next_free;
    g_next_free += nblocks;

    /* Data blocks (last padded with zeros). */
    uint8_t blk[SFS_BLOCK_SIZE];
    for (uint32_t e = 0; e < nblocks; e++) {
        uint32_t off  = e * SFS_BLOCK_SIZE;
        uint32_t part = (len - off > SFS_BLOCK_SIZE) ? SFS_BLOCK_SIZE : (len - off);
        memset(blk, 0, sizeof blk);
        memcpy(blk, data + off, part);
        wr_block(data_start + e, blk);
    }

    /* Inode block: one inline extent per data block. */
    memset(blk, 0, sizeof blk);
    struct sfs_inode *in = (struct sfs_inode *)blk;
    in->size = len;
    in->flags = 0;
    in->extent_count = (uint16_t)nblocks;
    for (uint32_t e = 0; e < nblocks; e++) {
        uint32_t off  = e * SFS_BLOCK_SIZE;
        uint32_t part = (len - off > SFS_BLOCK_SIZE) ? SFS_BLOCK_SIZE : (len - off);
        in->inline_extents[e].block_start = data_start + e;
        in->inline_extents[e].block_count = 1;
        in->inline_extents[e].logical_len = part;
        in->inline_extents[e].comp_len    = 0;
        in->inline_extents[e].flags       = 0;
    }
    wr_block(inode_blk, blk);

    /* Leaf slots: INODE (ino -> inode_blk) and DIR ((root<<32)|hash -> dirent). */
    struct { uint64_t inode_block; } ino_v = { inode_blk };
    leaf_add(SFS_KEY_INODE | ino, &ino_v, sizeof ino_v);

    struct sfs_dirent de;
    memset(&de, 0, sizeof de);
    de.inode_num = ino;
    de.name_len  = (uint8_t)nlen;
    memcpy(de.name, name, nlen);
    uint64_t dir_key = ((uint64_t)SFS_ROOT_INODE << 32)
                     | (uint64_t)sfs_name_hash32(name, (int)nlen);
    leaf_add(dir_key, &de, sizeof de);
}

static void sort_leaf(void) {   /* insertion sort by key (kernel bt_insert expects sorted) */
    for (int i = 1; i < g_root_leaf.nkeys; i++) {
        struct sfs_leaf_slot t = g_root_leaf.u.leaf[i];
        int j = i - 1;
        while (j >= 0 && g_root_leaf.u.leaf[j].key > t.key) {
            g_root_leaf.u.leaf[j + 1] = g_root_leaf.u.leaf[j];
            j--;
        }
        g_root_leaf.u.leaf[j + 1] = t;
    }
}

int main(int argc, char **argv) {
    const char *image = NULL;
    uint64_t blocks = DEFAULT_BLOCKS;
    const char *files[SFS_LEAF_MAX];
    int nfiles = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--blocks") && i + 1 < argc) {
            blocks = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--file") && i + 1 < argc) {
            if (nfiles >= (int)SFS_LEAF_MAX) { fprintf(stderr, "mkfs.sfs: too many --file\n"); return 1; }
            files[nfiles++] = argv[++i];
        } else if (!image) {
            image = argv[i];
        } else {
            fprintf(stderr, "mkfs.sfs: unexpected arg '%s'\n", argv[i]);
            return 1;
        }
    }
    if (!image) {
        fprintf(stderr, "usage: mkfs.sfs <image> [--blocks N] [--file NAME=hostpath] ...\n");
        return 1;
    }
    if (blocks < 8) { fprintf(stderr, "mkfs.sfs: need >= 8 blocks\n"); return 1; }

    g_fd = open(image, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (g_fd < 0) die("open image");
    if (ftruncate(g_fd, (off_t)(blocks * SFS_BLOCK_SIZE)) != 0) die("ftruncate");

    /* Root leaf (block 1): starts with the root-inode slot. */
    memset(&g_root_leaf, 0, sizeof g_root_leaf);
    g_root_leaf.flags = SFS_NODE_LEAF;
    g_root_leaf.generation = 1;
    struct { uint64_t inode_block; } root_ino_v = { 2 };
    leaf_add(SFS_KEY_INODE | SFS_ROOT_INODE, &root_ino_v, sizeof root_ino_v);

    /* Root inode (block 2): directory. */
    uint8_t blk[SFS_BLOCK_SIZE];
    memset(blk, 0, sizeof blk);
    ((struct sfs_inode *)blk)->flags = SFS_I_DIR;
    wr_block(2, blk);

    /* Journal (block 3): empty. */
    memset(blk, 0, sizeof blk);
    wr_block(3, blk);

    /* Provision files. */
    for (int i = 0; i < nfiles; i++) {
        const char *spec = files[i];
        const char *eq = strchr(spec, '=');
        if (!eq) { fprintf(stderr, "mkfs.sfs: --file needs NAME=path (got '%s')\n", spec); return 1; }
        size_t nlen = (size_t)(eq - spec);
        char name[256];
        if (nlen == 0 || nlen > 255) { fprintf(stderr, "mkfs.sfs: bad file name\n"); return 1; }
        memcpy(name, spec, nlen); name[nlen] = 0;

        int hf = open(eq + 1, O_RDONLY);
        if (hf < 0) { fprintf(stderr, "mkfs.sfs: open '%s': %s\n", eq + 1, strerror(errno)); return 1; }
        static uint8_t fbuf[MAX_FILE_BYTES + 1];
        ssize_t got = read(hf, fbuf, sizeof fbuf);
        close(hf);
        if (got < 0) die("read hostfile");
        if ((uint32_t)got > MAX_FILE_BYTES) {
            fprintf(stderr, "mkfs.sfs: '%s' exceeds %u B cap\n", name, MAX_FILE_BYTES);
            return 1;
        }
        add_root_file(name, fbuf, (uint32_t)got);
    }

    /* Finalize the root leaf (block 1). */
    sort_leaf();
    wr_block(1, &g_root_leaf);

    /* Superblock (block 0) — same fields as kernel sfs_format(). */
    memset(blk, 0, sizeof blk);
    struct sfs_superblock *sb = (struct sfs_superblock *)blk;
    sb->magic            = SFS_MAGIC;
    sb->version          = SFS_VERSION;
    sb->block_size       = SFS_BLOCK_SIZE;
    sb->total_blocks     = blocks;
    sb->root_btree       = 1;
    sb->generation       = 1;
    sb->next_free_block  = g_next_free;
    sb->next_inode       = g_next_inode;
    sb->txn_log_start    = 3;
    sb->txn_log_blocks   = 1;
    sb->free_block_count = blocks > g_next_free ? blocks - g_next_free : 0;
    wr_block(0, blk);

    if (close(g_fd) != 0) die("close");
    fprintf(stderr, "mkfs.sfs: %s — %llu blocks, %d file(s), next_free=%llu\n",
            image, (unsigned long long)blocks, nfiles,
            (unsigned long long)g_next_free);
    return 0;
}
