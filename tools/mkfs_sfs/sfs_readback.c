/* tools/mkfs_sfs/sfs_readback.c — host SFS read-back verifier (DDR-767 gate).
 *
 * Reads a root-level file out of an SFS image using the SAME on-disk structs
 * (kernel sfs.h) and the SAME read algorithm as the kernel (linear leaf scan for
 * the DIR key -> inode -> inline extents). Prints the file bytes to stdout, or a
 * diagnostic + non-zero exit on any miss. Used to prove mkfs.sfs output is
 * byte-decodable by the kernel reader without booting.
 *
 *   sfs_readback <image> <NAME>
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include "sfs.h"

static int fd;
static uint8_t b[SFS_BLOCK_SIZE];

static int rd(uint64_t i) {
    return pread(fd, b, SFS_BLOCK_SIZE, (off_t)(i * SFS_BLOCK_SIZE)) == (ssize_t)SFS_BLOCK_SIZE
           ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: sfs_readback <image> <NAME>\n"); return 2; }
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) { fprintf(stderr, "sfs_readback: open %s failed\n", argv[1]); return 2; }

    if (rd(0)) { fprintf(stderr, "sfs_readback: short read super\n"); return 2; }
    struct sfs_superblock *sb = (struct sfs_superblock *)b;
    if (sb->magic != SFS_MAGIC)   { fprintf(stderr, "sfs_readback: bad magic\n"); return 1; }
    if (sb->version != SFS_VERSION){ fprintf(stderr, "sfs_readback: bad version\n"); return 1; }
    uint64_t rootbt = sb->root_btree;

    const char *name = argv[2];
    struct sfs_node nd;

    /* DDR-773: descend internal nodes to the leaf that may hold `key`, exactly as
     * the kernel's bt_search_root does, and load it into `nd`. */
    #define LOAD_LEAF_FOR(key)                                                  \
        do {                                                                    \
            uint64_t _blk = rootbt;                                             \
            for (;;) {                                                          \
                if (rd(_blk)) return 2;                                         \
                memcpy(&nd, b, sizeof nd);                                      \
                if (nd.flags & SFS_NODE_LEAF) break;                            \
                int _i = 0;                                                     \
                while (_i < nd.nkeys && (key) >= nd.u.intern[_i].sep) _i++;      \
                _blk = (_i == 0) ? nd.child0 : nd.u.intern[_i - 1].child;        \
            }                                                                   \
        } while (0)

    /* Walk path components (matches kernel sfs_walk): each keyed by
     * (parent<<32)|hash, resolved via the linear leaf scan; last = the file. */
    uint64_t parent = SFS_ROOT_INODE, ino = 0, iblk = 0;
    const char *p = name;
    while (*p == '/') p++;
    for (;;) {
        int clen = 0;
        while (p[clen] && p[clen] != '/') clen++;
        if (clen == 0) { fprintf(stderr, "sfs_readback: bad path '%s'\n", name); return 1; }
        uint64_t dkey = (parent << 32) | (uint64_t)sfs_name_hash32(p, clen);
        LOAD_LEAF_FOR(dkey);
        uint64_t child = 0;
        for (int i = 0; i < nd.nkeys; i++)
            if (nd.u.leaf[i].key == dkey && nd.u.leaf[i].v.dir.inode_num &&
                nd.u.leaf[i].v.dir.name_len == clen &&
                memcmp(nd.u.leaf[i].v.dir.name, p, (size_t)clen) == 0)
                child = nd.u.leaf[i].v.dir.inode_num;
        if (!child) { fprintf(stderr, "sfs_readback: '%s' not found\n", name); return 1; }
        const char *next = p + clen;
        while (*next == '/') next++;
        if (*next == 0) { ino = child; break; }
        parent = child;
        p = next;
    }
    LOAD_LEAF_FOR(SFS_KEY_INODE | ino);
    for (int i = 0; i < nd.nkeys; i++)
        if (nd.u.leaf[i].key == (SFS_KEY_INODE | ino))
            iblk = nd.u.leaf[i].v.ino.inode_block;
    if (!iblk) { fprintf(stderr, "sfs_readback: inode %llu missing\n", (unsigned long long)ino); return 1; }

    struct sfs_inode in;
    if (rd(iblk)) return 2;
    memcpy(&in, b, sizeof in);

    /* Emit each inline extent's logical bytes in order. */
    for (int e = 0; e < in.extent_count && e < 4; e++) {
        struct sfs_extent_ref *x = &in.inline_extents[e];
        if (rd(x->block_start)) return 2;
        uint32_t n = x->logical_len > SFS_BLOCK_SIZE ? SFS_BLOCK_SIZE : x->logical_len;
        fwrite(b, 1, n, stdout);
    }
    return 0;
}
