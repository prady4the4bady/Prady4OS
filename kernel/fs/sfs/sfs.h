/* kernel/fs/sfs/sfs.h — SOVEREIGN Filesystem (SFS) skeleton (Phase 4 scaffold).
 *
 * SFS is PRADYOS's native filesystem: a copy-on-write B+ tree store with
 * versioned writes, atomic transactions (journal), 4 KiB metadata tags, and
 * inline LZ4 compression. NONE of that is implemented yet — this header pins the
 * on-disk layout and the driver registers a mount stub that recognises no
 * volume, so FAT32 remains the only concrete filesystem. The structs exist so
 * the VFS, capability classes, and tooling can be built against SFS before the
 * engine lands. See docs/decisions for the SFS design ADR (forthcoming).
 */
#pragma once
#include <stdint.h>

#define SFS_MAGIC      0x53465331ull   /* "SFS1" little-endian                */
#define SFS_VERSION    1u
#define SFS_BLOCK_SIZE 4096u           /* on-disk block / metadata-tag size   */
#define SFS_BTREE_FANOUT 64u           /* keys per B+ tree node (provisional) */

/* Feature bits negotiated at mount (none active yet). */
#define SFS_FEAT_VERSIONING (1u << 0)  /* snapshot roots retained             */
#define SFS_FEAT_TXN_LOG    (1u << 1)  /* journalled atomic transactions      */
#define SFS_FEAT_LZ4        (1u << 2)  /* inline LZ4 compression of extents   */

/* On-disk superblock (block 0). All fields little-endian on disk. */
struct sfs_superblock {
    uint64_t magic;                    /* SFS_MAGIC                           */
    uint32_t version;                  /* SFS_VERSION                         */
    uint32_t block_size;               /* SFS_BLOCK_SIZE                      */
    uint64_t total_blocks;             /* size of the volume in blocks        */
    uint64_t root_btree;               /* block # of the current B+ tree root */
    uint64_t snapshot_root;            /* most recent committed snapshot root */
    uint64_t free_extent_tree;         /* block # of the free-space B+ tree   */
    uint64_t txn_log_start;            /* first block of the transaction log  */
    uint64_t txn_log_blocks;           /* length of the transaction log       */
    uint64_t generation;              /* bumped on every committed transaction */
    uint32_t features;                 /* SFS_FEAT_* bitmap                    */
    uint32_t checksum;                 /* CRC32 of the superblock             */
    uint8_t  reserved[4016];           /* pad to a full 4 KiB block           */
};

/* B+ tree node (one block). Internal nodes point at child blocks; leaf nodes
 * carry inline values or extent references. Copy-on-write: an update writes a
 * new node block and rewrites the path to the root, enabling cheap snapshots. */
#define SFS_NODE_LEAF     (1u << 0)
#define SFS_NODE_INTERNAL (1u << 1)

struct sfs_btree_key {
    uint64_t key;                      /* object id / offset key              */
    uint64_t ptr;                      /* child block # (internal) or value   */
};

struct sfs_btree_node {
    uint16_t flags;                    /* SFS_NODE_LEAF | SFS_NODE_INTERNAL   */
    uint16_t nkeys;                    /* number of valid keys                */
    uint32_t checksum;                 /* CRC32 of the node                   */
    uint64_t next_leaf;                /* leaf chaining (0 if internal/last)  */
    uint64_t generation;               /* transaction that wrote this node    */
    struct sfs_btree_key keys[SFS_BTREE_FANOUT];
};

/* Register the SFS driver with the VFS. The mount probe currently recognises no
 * volume (returns failure), so registering it is harmless; it reserves the slot
 * and exercises the multi-filesystem mount path. */
void sfs_register(void);
