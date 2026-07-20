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

/* Scan the (single) root leaf for a live DIR slot named `name` under `parent`;
 * return its child inode, or 0 if absent. */
static uint64_t find_dir(uint64_t parent, const char *name, int nlen) {
    uint64_t key = (parent << 32) | (uint64_t)sfs_name_hash32(name, nlen);
    for (int i = 0; i < g_root_leaf.nkeys; i++) {
        struct sfs_leaf_slot *s = &g_root_leaf.u.leaf[i];
        if (s->key == key && s->v.dir.inode_num &&
            s->v.dir.name_len == nlen &&
            memcmp(s->v.dir.name, name, (size_t)nlen) == 0)
            return s->v.dir.inode_num;
    }
    return 0;
}

/* Append a DIR slot linking `name` under `parent` to inode `child`. */
static void link_dirent(uint64_t parent, const char *name, int nlen, uint64_t child) {
    struct sfs_dirent de;
    memset(&de, 0, sizeof de);
    de.inode_num = child;
    de.name_len  = (uint8_t)nlen;
    memcpy(de.name, name, (size_t)nlen);
    uint64_t key = (parent << 32) | (uint64_t)sfs_name_hash32(name, nlen);
    leaf_add(key, &de, sizeof de);
}

/* Find or create directory `name` under `parent`; return its inode number. A dir
 * inode is a block with SFS_INO_DIR and no extents — its contents are the
 * DIR-keyed slots parented to it (matches kernel sfs_do_create is_dir=1). */
static uint64_t find_or_make_dir(uint64_t parent, const char *name, int nlen) {
    uint64_t existing = find_dir(parent, name, nlen);
    if (existing) return existing;
    uint64_t ino  = g_next_inode++;
    uint64_t iblk = g_next_free++;
    uint8_t blk[SFS_BLOCK_SIZE];
    memset(blk, 0, sizeof blk);
    ((struct sfs_inode *)blk)->flags = SFS_INO_DIR;
    wr_block(iblk, blk);
    struct { uint64_t inode_block; } ino_v = { iblk };
    leaf_add(SFS_KEY_INODE | ino, &ino_v, sizeof ino_v);
    link_dirent(parent, name, nlen, ino);
    return ino;
}

/* Write a file inode (one inline extent per 4 KiB data block) and return it. */
static uint64_t write_file_inode(const uint8_t *data, uint32_t len) {
    uint64_t inode_blk  = g_next_free++;
    uint32_t nblocks    = len ? (len + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE : 0;
    uint64_t data_start = g_next_free;
    g_next_free += nblocks;

    uint8_t blk[SFS_BLOCK_SIZE];
    for (uint32_t e = 0; e < nblocks; e++) {
        uint32_t off  = e * SFS_BLOCK_SIZE;
        uint32_t part = (len - off > SFS_BLOCK_SIZE) ? SFS_BLOCK_SIZE : (len - off);
        memset(blk, 0, sizeof blk);
        memcpy(blk, data + off, part);
        wr_block(data_start + e, blk);
    }
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
    }
    wr_block(inode_blk, blk);
    return inode_blk;
}

/* Provision a file at `path` (may contain '/', creating intermediate dirs like
 * kernel sfs_walk: intermediates are directories, the last component is the
 * file). Slots share the single root leaf; dir prefixes are deduped. */
static void add_file(const char *path, const uint8_t *data, uint32_t len) {
    if (len > MAX_FILE_BYTES) {
        fprintf(stderr, "mkfs.sfs: '%s' is %u B > %u B cap (this slice)\n",
                path, len, MAX_FILE_BYTES);
        exit(1);
    }
    uint64_t parent = SFS_ROOT_INODE;
    const char *p = path;
    while (*p == '/') p++;
    for (;;) {
        int nlen = 0;
        while (p[nlen] && p[nlen] != '/') nlen++;
        if (nlen == 0 || nlen > 255) {
            fprintf(stderr, "mkfs.sfs: bad path '%s'\n", path); exit(1);
        }
        const char *next = p + nlen;
        while (*next == '/') next++;
        if (*next == 0) {                                  /* final = the file */
            uint64_t ino = g_next_inode++;
            uint64_t iblk = write_file_inode(data, len);
            struct { uint64_t inode_block; } ino_v = { iblk };
            leaf_add(SFS_KEY_INODE | ino, &ino_v, sizeof ino_v);
            link_dirent(parent, p, nlen, ino);
            return;
        }
        parent = find_or_make_dir(parent, p, nlen);        /* intermediate dir */
        p = next;
    }
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
        char path[256];   /* may contain '/' for nested provisioning (DDR-769) */
        if (nlen == 0 || nlen > 255) { fprintf(stderr, "mkfs.sfs: bad file path\n"); return 1; }
        memcpy(path, spec, nlen); path[nlen] = 0;

        int hf = open(eq + 1, O_RDONLY);
        if (hf < 0) { fprintf(stderr, "mkfs.sfs: open '%s': %s\n", eq + 1, strerror(errno)); return 1; }
        static uint8_t fbuf[MAX_FILE_BYTES + 1];
        ssize_t got = read(hf, fbuf, sizeof fbuf);
        close(hf);
        if (got < 0) die("read hostfile");
        if ((uint32_t)got > MAX_FILE_BYTES) {
            fprintf(stderr, "mkfs.sfs: '%s' exceeds %u B cap\n", path, MAX_FILE_BYTES);
            return 1;
        }
        add_file(path, fbuf, (uint32_t)got);
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
