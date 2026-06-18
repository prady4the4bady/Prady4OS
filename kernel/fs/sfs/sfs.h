/* kernel/fs/sfs/sfs.h — SOVEREIGN Filesystem (SFS) on-disk format + API.
 *
 * Inode-based copy-on-write B+ tree store (ADR-018). 4 KiB blocks. The B+tree
 * is keyed by sorted uint64 with type flags in the high bits (DIR/INODE/EXTENT).
 * Inodes live in their own 4 KiB blocks (header + ~4 KiB metadata tag); the
 * INODE-keyed tree entry holds the inode block pointer so CoW can relocate it.
 * Slices: 4d format/mount; 4e B+tree create/lookup/open; 4f extents r/w;
 * 4g journal; 4h snapshots; 4i LZ4 + tags.
 */
#pragma once
#include <stdint.h>

struct blk_device;                     /* kernel/drivers/blk/blk.h */

#define SFS_MAGIC      0x53465331ull   /* "SFS1" little-endian                 */
#define SFS_VERSION    2u              /* v2: B+tree + inodes (slice 4e)        */
#define SFS_BLOCK_SIZE 4096u
#define SFS_SECTORS_PER_BLOCK (SFS_BLOCK_SIZE / 512u)   /* 8 sectors/block      */

/* B+tree key-type flags (top bits of the uint64 key). */
#define SFS_KEY_TYPE_MASK 0xC000000000000000ull
#define SFS_KEY_DIR       0x0000000000000000ull   /* (parent_inode<<32)|hash32  */
#define SFS_KEY_INODE     0x8000000000000000ull   /* | inode_number             */
#define SFS_KEY_EXTENT    0x4000000000000000ull   /* | (inode<<20)|block_idx    */

/* Inode flags. */
#define SFS_I_DIR   (1u << 0)
#define SFS_I_LZ4   (1u << 1)          /* extents are LZ4-compressed (slice 4i)  */

#define SFS_ROOT_INODE 1ull            /* inode number of the root directory     */

/* Feature bits negotiated at mount (none active yet). */
#define SFS_FEAT_VERSIONING (1u << 0)
#define SFS_FEAT_TXN_LOG    (1u << 1)
#define SFS_FEAT_LZ4        (1u << 2)

/* Superblock (block 0), padded to a full 4 KiB block. */
struct sfs_superblock {
    uint64_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t root_btree;               /* block # of the current B+ tree root   */
    uint64_t snapshot_root;            /* latest committed snapshot root (4h)    */
    uint64_t free_extent_tree;         /* free-space B+ tree root (4h; 0 = none) */
    uint64_t txn_log_start;            /* journal first block (4g)               */
    uint32_t txn_log_blocks;           /* journal length in blocks (4g)          */
    uint32_t features;                 /* SFS_FEAT_*                             */
    uint64_t generation;               /* bumped on every committed transaction  */
    uint64_t next_free_block;          /* high-water allocator until free tree   */
    uint64_t next_inode;               /* next inode number to hand out          */
    uint64_t free_block_count;         /* informational free-block estimate      */
    uint32_t checksum;                 /* CRC32 of the superblock (0 = unset)    */
    uint8_t  reserved[3996];           /* pad to 4096 (100 B of fields above)    */
};

/* B+tree node types. */
#define SFS_NODE_LEAF     1u
#define SFS_NODE_INTERNAL 2u

/* Leaf slot value, interpreted by the key type. */
struct sfs_dirent {                    /* DIR */
    uint64_t inode_num;
    uint8_t  name_len;
    char     name[255];
};                                     /* 264 bytes */

struct sfs_leaf_slot {
    uint64_t key;
    union {
        struct sfs_dirent dir;                          /* SFS_KEY_DIR    */
        struct { uint64_t inode_block; } ino;           /* SFS_KEY_INODE  */
        struct { uint64_t start; uint32_t count; } ext; /* SFS_KEY_EXTENT */
        uint8_t raw[264];
    } v;
};                                     /* 8 + 264 = 272 bytes */

/* Internal slot: separator key + the child that holds keys >= it. The leftmost
 * child (keys < slot[0].key) is stored in the node's `child0` field. */
struct sfs_int_slot {
    uint64_t sep;
    uint64_t child;
};                                     /* 16 bytes */

#define SFS_NODE_HDR_BYTES 32u         /* flags..child0, before the slot union   */
#define SFS_LEAF_MAX  ((SFS_BLOCK_SIZE - SFS_NODE_HDR_BYTES) / sizeof(struct sfs_leaf_slot)) /* 14  */
#define SFS_INT_MAX   ((SFS_BLOCK_SIZE - SFS_NODE_HDR_BYTES) / sizeof(struct sfs_int_slot))  /* 254 */

struct sfs_node {
    uint16_t flags;                    /* SFS_NODE_LEAF | SFS_NODE_INTERNAL */
    uint16_t nkeys;
    uint32_t reserved0;
    uint64_t next_leaf;                /* leaf chaining (0 if none/internal)    */
    uint64_t generation;
    uint64_t child0;                   /* internal: leftmost child block         */
    union {
        struct sfs_leaf_slot leaf[SFS_LEAF_MAX];
        struct sfs_int_slot  intern[SFS_INT_MAX];
        uint8_t raw[SFS_BLOCK_SIZE - SFS_NODE_HDR_BYTES];
    } u;
};                                     /* exactly SFS_BLOCK_SIZE */

/* Inode record — its own 4 KiB block. */
struct sfs_extent_ref {
    uint64_t block_start;
    uint32_t block_count;
    uint32_t reserved;
};                                     /* 16 bytes */

struct sfs_inode {
    uint64_t size;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t ctime;
    uint64_t mtime;
    uint16_t extent_count;
    uint16_t reserved1;
    uint32_t reserved2;
    struct sfs_extent_ref inline_extents[4];           /* 64 bytes */
    uint8_t  tag[SFS_BLOCK_SIZE - (8+4+4+8+8+2+2+4+64)]; /* ~3992 B metadata tag */
};                                     /* exactly SFS_BLOCK_SIZE */

/* Hash a name component to 32 bits (FNV-1a). */
static inline uint32_t sfs_name_hash32(const char *s, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

/* Register the SFS driver with the VFS (probe claims a disk iff block 0 is a
 * valid SFS superblock). */
void sfs_register(void);

/* Format `bd` as an empty SFS volume (superblock + empty root leaf + root-dir
 * inode). In-kernel mkfs; conceptually CAP_FS_SFS_ADMIN. 0 on success. */
int  sfs_format(struct blk_device *bd);
