/* kernel/fs/sfs/sfs.c — SOVEREIGN Filesystem (SFS).
 *
 * Slice 4d: format / mount / empty root.
 * Slice 4e: inode-based copy-on-write B+ tree — create, lookup, open, readdir.
 *   Inserts never mutate a live node: the path from the affected leaf to the
 *   root is rebuilt into freshly allocated blocks (split on overflow) and the
 *   new root is published by writing the superblock (single-block commit). On a
 *   crash before that write, the old superblock still names the old root, so the
 *   uncommitted blocks are simply forgotten (journal hardening lands in 4g).
 * File data extents (read/write) are slice 4f — they remain stubs here.
 */
#include "sfs.h"
#include "vfs.h"
#include "blk.h"
#include "pmm.h"
#include "kheap.h"
#include "console.h"
#include "string.h"

_Static_assert(sizeof(struct sfs_superblock) == SFS_BLOCK_SIZE, "sfs superblock != 4096");
_Static_assert(sizeof(struct sfs_node)       == SFS_BLOCK_SIZE, "sfs node != 4096");
_Static_assert(sizeof(struct sfs_inode)      == SFS_BLOCK_SIZE, "sfs inode != 4096");
_Static_assert(sizeof(struct sfs_leaf_slot)  == 272,            "sfs leaf slot != 272");

/* Logical commit-record journal (ADR-018 slice 4g). SFS is copy-on-write, so a
 * transaction's new blocks are already written out of place; the journal only
 * needs to make the *root swap* crash-atomic. txn_commit writes this record
 * (CRC-protected) before the superblock; if the superblock write does not land,
 * mount sees journal.txn_id > superblock.generation and replays it. */
#define SFS_JOURNAL_MAGIC 0x534a4e31u   /* "SJN1" */
struct sfs_journal_rec {
    uint32_t magic;
    uint32_t crc32;                     /* CRC32 over the 40 bytes that follow  */
    uint64_t txn_id;                    /* generation this commit publishes     */
    uint64_t root_btree;
    uint64_t next_free;
    uint64_t next_inode;
    uint64_t free_block_count;
    uint8_t  reserved[SFS_BLOCK_SIZE - 48];
};
_Static_assert(sizeof(struct sfs_journal_rec) == SFS_BLOCK_SIZE, "sfs journal != 4096");

static uint32_t sfs_crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

struct sfs_ctx {
    struct blk_device *bd;
    uint64_t total_blocks;
    uint64_t root_btree;
    uint64_t next_free;
    uint64_t next_inode;
    uint64_t generation;
    uint64_t txn_log_start;            /* journal block                         */
    int      in_txn;                   /* transaction open: defer superblock     */
    uint64_t saved_root;               /* rollback point for txn_abort           */
    uint64_t saved_next_free;
    uint64_t saved_next_inode;
    uint32_t snapshot_count;
    struct sfs_snap snapshots[SFS_MAX_SNAPSHOTS];
};

/* ---- block I/O (one 4 KiB block = 8 sectors; buffers are PMM pages) ------- */
/* Bare device I/O, usable before a ctx exists (mount/format). */
static void rd_block_bd(struct blk_device *bd, uint64_t blk, void *buf) {
    bd->read(bd, blk * SFS_SECTORS_PER_BLOCK, buf, SFS_SECTORS_PER_BLOCK);
}
static void wr_block_bd(struct blk_device *bd, uint64_t blk, const void *buf) {
    bd->write(bd, blk * SFS_SECTORS_PER_BLOCK, buf, SFS_SECTORS_PER_BLOCK);
}
static void rd_block(struct sfs_ctx *c, uint64_t blk, void *buf) {
    rd_block_bd(c->bd, blk, buf);
}
static void wr_block(struct sfs_ctx *c, uint64_t blk, const void *buf) {
    wr_block_bd(c->bd, blk, buf);
}

/* High-water allocator (free-space tree arrives in slice 4h). */
static uint64_t alloc_block(struct sfs_ctx *c) {
    return c->next_free++;
}

/* Write the superblock from the current in-memory state (the commit point),
 * bumping the generation. */
static void sfs_write_super(struct sfs_ctx *c) {
    uint64_t page = pmm_alloc_page();
    if (!page)
        return;
    struct sfs_superblock *sb = (struct sfs_superblock *)(uintptr_t)page;
    memset(sb, 0, SFS_BLOCK_SIZE);
    sb->magic            = SFS_MAGIC;
    sb->version          = SFS_VERSION;
    sb->block_size       = SFS_BLOCK_SIZE;
    sb->total_blocks     = c->total_blocks;
    sb->root_btree       = c->root_btree;
    sb->generation       = ++c->generation;
    sb->next_free_block  = c->next_free;
    sb->next_inode       = c->next_inode;
    sb->txn_log_start    = c->txn_log_start;
    sb->txn_log_blocks   = 1;
    sb->free_block_count = (c->total_blocks > c->next_free)
                         ? (c->total_blocks - c->next_free) : 0;
    sb->snapshot_count   = c->snapshot_count;
    for (uint32_t i = 0; i < SFS_MAX_SNAPSHOTS; i++)
        sb->snapshots[i] = c->snapshots[i];
    wr_block(c, 0, sb);
    pmm_free_page(page);
}

/* Per-operation commit. Inside a transaction the superblock write is deferred to
 * txn_commit, so a sequence of operations publishes atomically. */
static void sfs_commit(struct sfs_ctx *c) {
    if (c->in_txn)
        return;
    sfs_write_super(c);
}

/* Write the journal commit record (the intended new root + counters) ahead of
 * the superblock. */
static void sfs_journal_write(struct sfs_ctx *c) {
    uint64_t page = pmm_alloc_page();
    if (!page)
        return;
    struct sfs_journal_rec *j = (struct sfs_journal_rec *)(uintptr_t)page;
    memset(j, 0, SFS_BLOCK_SIZE);
    j->magic            = SFS_JOURNAL_MAGIC;
    j->txn_id           = c->generation + 1;     /* generation this commit gets */
    j->root_btree       = c->root_btree;
    j->next_free        = c->next_free;
    j->next_inode       = c->next_inode;
    j->free_block_count = (c->total_blocks > c->next_free)
                        ? (c->total_blocks - c->next_free) : 0;
    j->crc32            = sfs_crc32((const uint8_t *)j + 8, 40);
    wr_block(c, c->txn_log_start, j);
    pmm_free_page(page);
}

static int sfs_txn_begin(void *ctx) {
    struct sfs_ctx *c = (struct sfs_ctx *)ctx;
    if (c->in_txn)
        return -1;
    c->saved_root       = c->root_btree;
    c->saved_next_free  = c->next_free;
    c->saved_next_inode = c->next_inode;
    c->in_txn = 1;
    return 0;
}

static int sfs_txn_commit(void *ctx) {
    struct sfs_ctx *c = (struct sfs_ctx *)ctx;
    if (!c->in_txn)
        return -1;
    c->in_txn = 0;
    sfs_journal_write(c);          /* write-ahead: record the root swap first   */
    sfs_write_super(c);            /* then publish (the checkpoint)             */
    return 0;
}

static int sfs_txn_abort(void *ctx) {
    struct sfs_ctx *c = (struct sfs_ctx *)ctx;
    if (!c->in_txn)
        return -1;
    c->root_btree = c->saved_root; /* discard uncommitted CoW blocks (forgotten) */
    c->next_free  = c->saved_next_free;
    c->next_inode = c->saved_next_inode;
    c->in_txn = 0;
    return 0;                       /* superblock untouched: nothing persisted   */
}

static void sfs_umount(void *ctx) {
    kfree(ctx);
}

/* ---- copy-on-write B+ tree ----------------------------------------------- */

struct ins_res {
    int      split;        /* 0: single new block in `left`; 1: split        */
    uint64_t left;         /* new (left) block                               */
    uint64_t sep;          /* split: separator key promoted to parent        */
    uint64_t right;        /* split: new right block                         */
};

static int bt_insert_rec(struct sfs_ctx *c, uint64_t blk,
                         const struct sfs_leaf_slot *item, struct ins_res *res) {
    uint64_t np = pmm_alloc_page();
    if (!np)
        return -1;
    struct sfs_node *n = (struct sfs_node *)(uintptr_t)np;
    rd_block(c, blk, n);

    if (n->flags & SFS_NODE_LEAF) {
        /* Build the sorted entry list with `item` inserted (replace on equal). */
        uint64_t wp = pmm_alloc_page();
        if (!wp) { pmm_free_page(np); return -1; }
        struct sfs_leaf_slot *w = (struct sfs_leaf_slot *)(uintptr_t)wp;
        int cnt = 0, placed = 0;
        for (int i = 0; i < n->nkeys; i++) {
            if (!placed && item->key <= n->u.leaf[i].key) {
                int eq = (item->key == n->u.leaf[i].key);
                w[cnt++] = *item;
                placed = 1;
                if (eq) continue;          /* replace existing key */
            }
            w[cnt++] = n->u.leaf[i];
        }
        if (!placed)
            w[cnt++] = *item;

        uint64_t ob = pmm_alloc_page();
        if (!ob) { pmm_free_page(wp); pmm_free_page(np); return -1; }
        struct sfs_node *o = (struct sfs_node *)(uintptr_t)ob;

        if (cnt <= (int)SFS_LEAF_MAX) {
            uint64_t nb = alloc_block(c);
            memset(o, 0, SFS_BLOCK_SIZE);
            o->flags = SFS_NODE_LEAF;
            o->nkeys = (uint16_t)cnt;
            o->next_leaf = n->next_leaf;
            o->generation = c->generation + 1;
            for (int i = 0; i < cnt; i++) o->u.leaf[i] = w[i];
            wr_block(c, nb, o);
            res->split = 0; res->left = nb;
        } else {
            int half = cnt / 2;            /* left [0,half), right [half,cnt) */
            uint64_t lb = alloc_block(c), rb = alloc_block(c);
            memset(o, 0, SFS_BLOCK_SIZE);
            o->flags = SFS_NODE_LEAF;
            o->nkeys = (uint16_t)(cnt - half);
            o->next_leaf = n->next_leaf;
            o->generation = c->generation + 1;
            for (int i = half; i < cnt; i++) o->u.leaf[i - half] = w[i];
            wr_block(c, rb, o);
            memset(o, 0, SFS_BLOCK_SIZE);
            o->flags = SFS_NODE_LEAF;
            o->nkeys = (uint16_t)half;
            o->next_leaf = rb;
            o->generation = c->generation + 1;
            for (int i = 0; i < half; i++) o->u.leaf[i] = w[i];
            wr_block(c, lb, o);
            res->split = 1; res->left = lb; res->sep = w[half].key; res->right = rb;
        }
        pmm_free_page(ob); pmm_free_page(wp); pmm_free_page(np);
        return 0;
    }

    /* Internal node: descend to the right child, then rebuild this node CoW. */
    int ci = 0;
    while (ci < n->nkeys && item->key >= n->u.intern[ci].sep) ci++;
    uint64_t child = (ci == 0) ? n->child0 : n->u.intern[ci - 1].child;

    struct ins_res cr;
    if (bt_insert_rec(c, child, item, &cr)) { pmm_free_page(np); return -1; }

    /* Expand into a working child/separator list, apply the child's result. */
    uint64_t wp = pmm_alloc_page();
    if (!wp) { pmm_free_page(np); return -1; }
    uint64_t *children = (uint64_t *)(uintptr_t)wp;   /* [0..255]  */
    uint64_t *seps     = children + 256;              /* [0..253]  */
    int nch = 0, nsep = 0;
    children[nch++] = n->child0;
    for (int k = 0; k < n->nkeys; k++) {
        seps[nsep++] = n->u.intern[k].sep;
        children[nch++] = n->u.intern[k].child;
    }
    if (!cr.split) {
        children[ci] = cr.left;
    } else {
        children[ci] = cr.left;
        for (int k = nsep; k > ci; k--) seps[k] = seps[k - 1];
        seps[ci] = cr.sep; nsep++;
        for (int k = nch; k > ci + 1; k--) children[k] = children[k - 1];
        children[ci + 1] = cr.right; nch++;
    }

    uint64_t ob = pmm_alloc_page();
    if (!ob) { pmm_free_page(wp); pmm_free_page(np); return -1; }
    struct sfs_node *o = (struct sfs_node *)(uintptr_t)ob;

    if (nsep <= (int)SFS_INT_MAX) {
        uint64_t nb = alloc_block(c);
        memset(o, 0, SFS_BLOCK_SIZE);
        o->flags = SFS_NODE_INTERNAL;
        o->nkeys = (uint16_t)nsep;
        o->generation = c->generation + 1;
        o->child0 = children[0];
        for (int k = 0; k < nsep; k++) {
            o->u.intern[k].sep = seps[k];
            o->u.intern[k].child = children[k + 1];
        }
        wr_block(c, nb, o);
        res->split = 0; res->left = nb;
    } else {
        int mid = nsep / 2;
        uint64_t promo = seps[mid];
        uint64_t lb = alloc_block(c), rb = alloc_block(c);
        memset(o, 0, SFS_BLOCK_SIZE);
        o->flags = SFS_NODE_INTERNAL;
        o->nkeys = (uint16_t)mid;
        o->generation = c->generation + 1;
        o->child0 = children[0];
        for (int k = 0; k < mid; k++) {
            o->u.intern[k].sep = seps[k];
            o->u.intern[k].child = children[k + 1];
        }
        wr_block(c, lb, o);
        int rsep = nsep - mid - 1;
        memset(o, 0, SFS_BLOCK_SIZE);
        o->flags = SFS_NODE_INTERNAL;
        o->nkeys = (uint16_t)rsep;
        o->generation = c->generation + 1;
        o->child0 = children[mid + 1];
        for (int k = 0; k < rsep; k++) {
            o->u.intern[k].sep = seps[mid + 1 + k];
            o->u.intern[k].child = children[mid + 2 + k];
        }
        wr_block(c, rb, o);
        res->split = 1; res->left = lb; res->sep = promo; res->right = rb;
    }
    pmm_free_page(ob); pmm_free_page(wp); pmm_free_page(np);
    return 0;
}

/* Insert one slot, updating the in-memory root (caller commits). */
static int bt_insert(struct sfs_ctx *c, const struct sfs_leaf_slot *item) {
    struct ins_res r;
    if (bt_insert_rec(c, c->root_btree, item, &r))
        return -1;
    if (!r.split) {
        c->root_btree = r.left;
        return 0;
    }
    uint64_t nr = alloc_block(c);
    uint64_t ob = pmm_alloc_page();
    if (!ob) return -1;
    struct sfs_node *o = (struct sfs_node *)(uintptr_t)ob;
    memset(o, 0, SFS_BLOCK_SIZE);
    o->flags = SFS_NODE_INTERNAL;
    o->nkeys = 1;
    o->generation = c->generation + 1;
    o->child0 = r.left;
    o->u.intern[0].sep = r.sep;
    o->u.intern[0].child = r.right;
    wr_block(c, nr, o);
    pmm_free_page(ob);
    c->root_btree = nr;
    return 0;
}

/* Exact-key lookup from an explicit B+ tree root (used for versioned reads). */
static int bt_search_root(struct sfs_ctx *c, uint64_t root, uint64_t key,
                          struct sfs_leaf_slot *out) {
    uint64_t np = pmm_alloc_page();
    if (!np)
        return -1;
    struct sfs_node *n = (struct sfs_node *)(uintptr_t)np;
    uint64_t blk = root;
    for (;;) {
        rd_block(c, blk, n);
        if (n->flags & SFS_NODE_LEAF) {
            for (int i = 0; i < n->nkeys; i++) {
                if (n->u.leaf[i].key == key) {
                    *out = n->u.leaf[i];
                    pmm_free_page(np);
                    return 0;
                }
            }
            pmm_free_page(np);
            return -1;
        }
        int i = 0;
        while (i < n->nkeys && key >= n->u.intern[i].sep) i++;
        blk = (i == 0) ? n->child0 : n->u.intern[i - 1].child;
    }
}

/* Exact-key lookup in the current tree. */
static int bt_search(struct sfs_ctx *c, uint64_t key, struct sfs_leaf_slot *out) {
    return bt_search_root(c, c->root_btree, key, out);
}

/* ---- directory / inode helpers ------------------------------------------- */

static int name_len_of(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int sfs_do_lookup(struct sfs_ctx *c, uint64_t parent,
                         const char *name, int len, uint64_t *out_inode) {
    if (len <= 0 || len > 255)
        return -1;
    uint64_t key = (parent << 32) | (uint64_t)sfs_name_hash32(name, len);
    struct sfs_leaf_slot s;
    if (bt_search(c, key, &s) != 0)
        return -1;
    if (s.v.dir.name_len != len)               /* hash-collision guard */
        return -1;
    for (int i = 0; i < len; i++)
        if (s.v.dir.name[i] != name[i])
            return -1;
    *out_inode = s.v.dir.inode_num;
    return 0;
}

static int sfs_do_create(struct sfs_ctx *c, uint64_t parent,
                         const char *name, int len, uint64_t *out_inode) {
    if (len <= 0 || len > 255)
        return -1;
    uint64_t dir_key = (parent << 32) | (uint64_t)sfs_name_hash32(name, len);
    struct sfs_leaf_slot probe;
    if (bt_search(c, dir_key, &probe) == 0)
        return -1;                              /* already exists */

    uint64_t ino  = c->next_inode++;
    uint64_t iblk = alloc_block(c);

    uint64_t ip = pmm_alloc_page();
    if (!ip) return -1;
    struct sfs_inode *in = (struct sfs_inode *)(uintptr_t)ip;
    memset(in, 0, SFS_BLOCK_SIZE);
    wr_block(c, iblk, in);
    pmm_free_page(ip);

    struct sfs_leaf_slot s;
    memset(&s, 0, sizeof s);
    s.key = SFS_KEY_INODE | ino;
    s.v.ino.inode_block = iblk;
    if (bt_insert(c, &s)) return -1;

    memset(&s, 0, sizeof s);
    s.key = dir_key;
    s.v.dir.inode_num = ino;
    s.v.dir.name_len = (uint8_t)len;
    for (int i = 0; i < len; i++) s.v.dir.name[i] = name[i];
    if (bt_insert(c, &s)) return -1;

    sfs_commit(c);
    if (out_inode) *out_inode = ino;
    return 0;
}

/* ---- VFS operations ------------------------------------------------------ */

static const char *skip_slashes(const char *p) {
    while (*p == '/') p++;
    return p;
}

static int sfs_open(void *ctx, const char *path, struct vfs_file *out) {
    struct sfs_ctx *c = (struct sfs_ctx *)ctx;
    const char *name = skip_slashes(path);
    int len = name_len_of(name);
    if (len == 0)
        return -1;                              /* root dir, not a file */
    uint64_t ino;
    if (sfs_do_lookup(c, SFS_ROOT_INODE, name, len, &ino) != 0)
        return -1;
    struct sfs_leaf_slot s;
    if (bt_search(c, SFS_KEY_INODE | ino, &s) != 0)
        return -1;
    uint64_t ip = pmm_alloc_page();
    if (!ip) return -1;
    struct sfs_inode *in = (struct sfs_inode *)(uintptr_t)ip;
    rd_block(c, s.v.ino.inode_block, in);
    out->size = in->size;
    out->cookie = (uint32_t)ino;
    out->dirent_clus = 0;
    out->dirent_off = 0;
    pmm_free_page(ip);
    return 0;
}

static int sfs_create (void *ctx, const char *path, struct vfs_file *out) {
    struct sfs_ctx *c = (struct sfs_ctx *)ctx;
    const char *name = skip_slashes(path);
    int len = name_len_of(name);
    uint64_t ino;
    if (sfs_do_create(c, SFS_ROOT_INODE, name, len, &ino) != 0)
        return -1;
    out->size = 0;
    out->cookie = (uint32_t)ino;
    out->dirent_clus = 0;
    out->dirent_off = 0;
    return 0;
}

/* In-order tree walk collecting DIR entries for `parent`. Returns 1 when the
 * `index`-th entry is found (fills name), 0 if not in this subtree, -1 on OOM.
 * Uses the live tree structure (not next_leaf chaining, which CoW leaves stale). */
static int sfs_dir_walk(struct sfs_ctx *c, uint64_t blk, uint64_t parent,
                        int index, int *count, char *name) {
    uint64_t np = pmm_alloc_page();
    if (!np)
        return -1;
    struct sfs_node *n = (struct sfs_node *)(uintptr_t)np;
    rd_block(c, blk, n);

    if (n->flags & SFS_NODE_LEAF) {
        for (int i = 0; i < n->nkeys; i++) {
            uint64_t k = n->u.leaf[i].key;
            if ((k & SFS_KEY_TYPE_MASK) == SFS_KEY_DIR && (k >> 32) == parent) {
                if (*count == index) {
                    int l = n->u.leaf[i].v.dir.name_len;
                    for (int j = 0; j < l; j++) name[j] = n->u.leaf[i].v.dir.name[j];
                    name[l] = 0;
                    pmm_free_page(np);
                    return 1;
                }
                (*count)++;
            }
        }
        pmm_free_page(np);
        return 0;
    }

    uint64_t kids[SFS_INT_MAX + 1];
    int nk = 0;
    kids[nk++] = n->child0;
    for (int i = 0; i < n->nkeys; i++)
        kids[nk++] = n->u.intern[i].child;
    pmm_free_page(np);                          /* free before descending */

    for (int i = 0; i < nk; i++) {
        int r = sfs_dir_walk(c, kids[i], parent, index, count, name);
        if (r != 0)
            return r;                           /* found (1) or error (-1) */
    }
    return 0;
}

static int sfs_readdir(void *ctx, const char *path, int index, char *name, uint32_t *size) {
    struct sfs_ctx *c = (struct sfs_ctx *)ctx;
    (void)path;
    int count = 0;
    int r = sfs_dir_walk(c, c->root_btree, SFS_ROOT_INODE, index, &count, name);
    if (r != 1)
        return -1;
    if (size) *size = 0;
    return 0;
}

/* Read the inode block for inode `ino` from B+ tree `root`; 0 on miss. */
static uint64_t inode_block_of_root(struct sfs_ctx *c, uint64_t root, uint64_t ino,
                                    struct sfs_inode *in) {
    struct sfs_leaf_slot s;
    if (bt_search_root(c, root, SFS_KEY_INODE | ino, &s) != 0)
        return 0;
    rd_block(c, s.v.ino.inode_block, in);
    return s.v.ino.inode_block;
}
static uint64_t inode_block_of(struct sfs_ctx *c, uint64_t ino, struct sfs_inode *in) {
    return inode_block_of_root(c, c->root_btree, ino, in);
}

/* Read file bytes by walking the inode's inline extents in order (slice 4f).
 * A versioned handle (vfs_file.dirent_clus != 0) reads from that snapshot root. */
static int sfs_read(void *ctx, const struct vfs_file *f, uint64_t off, void *buf, uint32_t len) {
    struct sfs_ctx *c = (struct sfs_ctx *)ctx;
    uint64_t root = f->dirent_clus ? f->dirent_clus : c->root_btree;
    uint64_t ip = pmm_alloc_page();
    if (!ip) return -1;
    struct sfs_inode *in = (struct sfs_inode *)(uintptr_t)ip;
    if (!inode_block_of_root(c, root, f->cookie, in)) { pmm_free_page(ip); return -1; }
    uint64_t fsize = in->size;

    uint64_t db = pmm_alloc_page();
    if (!db) { pmm_free_page(ip); return -1; }
    uint8_t *dbuf = (uint8_t *)(uintptr_t)db;
    uint8_t *out = (uint8_t *)buf;
    uint32_t copied = 0;
    uint64_t fpos = 0;                          /* file offset at block start */
    for (int e = 0; e < in->extent_count && copied < len; e++) {
        uint64_t bs = in->inline_extents[e].block_start;
        uint32_t bc = in->inline_extents[e].block_count;
        for (uint32_t b = 0; b < bc && copied < len; b++) {
            if (fpos + SFS_BLOCK_SIZE > off && fpos < off + len && fpos < fsize) {
                rd_block(c, bs + b, dbuf);
                for (uint32_t i = 0; i < SFS_BLOCK_SIZE && copied < len; i++) {
                    uint64_t fo = fpos + i;
                    if (fo >= off && fo < off + len && fo < fsize)
                        out[copied++] = dbuf[i];
                }
            }
            fpos += SFS_BLOCK_SIZE;
        }
    }
    pmm_free_page(db);
    pmm_free_page(ip);
    return (int)copied;
}

/* Append/grow write (slice 4f): allocate a contiguous extent for [off,off+len),
 * CoW the inode to a new block, and repoint its INODE entry. `off` must equal
 * the current file size (mid-file overwrite is a later slice). */
static int sfs_write(void *ctx, struct vfs_file *f, uint64_t off, const void *buf, uint32_t len) {
    struct sfs_ctx *c = (struct sfs_ctx *)ctx;
    if (len == 0)
        return 0;
    if (f->dirent_clus != 0)
        return -1;                               /* versioned handle is read-only */
    uint64_t ip = pmm_alloc_page();
    if (!ip) return -1;
    struct sfs_inode *in = (struct sfs_inode *)(uintptr_t)ip;
    if (!inode_block_of(c, f->cookie, in)) { pmm_free_page(ip); return -1; }

    if (off != in->size || in->extent_count >= 4) {
        pmm_free_page(ip);                       /* overwrite / >4 extents: later */
        return -1;
    }

    uint32_t nblocks = (len + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE;
    uint64_t start = c->next_free;               /* contiguous (high-water) */

    uint64_t db = pmm_alloc_page();
    if (!db) { pmm_free_page(ip); return -1; }
    uint8_t *dbuf = (uint8_t *)(uintptr_t)db;
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t done = 0;
    for (uint32_t i = 0; i < nblocks; i++) {
        uint64_t blk = alloc_block(c);           /* == start + i */
        uint32_t chunk = (len - done < SFS_BLOCK_SIZE) ? (len - done) : SFS_BLOCK_SIZE;
        memset(dbuf, 0, SFS_BLOCK_SIZE);
        memcpy(dbuf, src + done, chunk);
        wr_block(c, blk, dbuf);
        done += chunk;
    }
    pmm_free_page(db);

    in->inline_extents[in->extent_count].block_start = start;
    in->inline_extents[in->extent_count].block_count = nblocks;
    in->extent_count++;
    in->size = off + len;

    uint64_t niblk = alloc_block(c);             /* CoW the inode */
    wr_block(c, niblk, in);
    pmm_free_page(ip);

    struct sfs_leaf_slot s;
    memset(&s, 0, sizeof s);
    s.key = SFS_KEY_INODE | f->cookie;
    s.v.ino.inode_block = niblk;
    if (bt_insert(c, &s))                         /* replaces the INODE entry */
        return -1;
    sfs_commit(c);
    f->size = off + len;
    return (int)len;
}

static int sfs_unlink(void *ctx, const char *path) {
    (void)ctx; (void)path; return -1;            /* slice TBD */
}

/* ---- mount / format / register ------------------------------------------- */

static int sfs_mount(struct blk_device *bd, void **ctx) {
    if (!bd)
        return -1;
    uint64_t page = pmm_alloc_page();
    if (!page)
        return -1;
    rd_block_bd(bd, 0, (void *)(uintptr_t)page);
    struct sfs_superblock *sb = (struct sfs_superblock *)(uintptr_t)page;
    if (sb->magic != SFS_MAGIC || sb->block_size != SFS_BLOCK_SIZE) {
        pmm_free_page(page);
        return -1;
    }
    struct sfs_ctx *c = (struct sfs_ctx *)kmalloc(sizeof(struct sfs_ctx));
    if (!c) { pmm_free_page(page); return -1; }
    c->bd          = bd;
    c->total_blocks = sb->total_blocks;
    c->root_btree  = sb->root_btree;
    c->next_free   = sb->next_free_block;
    c->next_inode  = sb->next_inode;
    c->generation  = sb->generation;
    c->txn_log_start = sb->txn_log_start;
    c->in_txn      = 0;
    c->saved_root = c->saved_next_free = c->saved_next_inode = 0;
    c->snapshot_count = sb->snapshot_count;
    if (c->snapshot_count > SFS_MAX_SNAPSHOTS)
        c->snapshot_count = SFS_MAX_SNAPSHOTS;
    for (uint32_t i = 0; i < SFS_MAX_SNAPSHOTS; i++)
        c->snapshots[i] = sb->snapshots[i];
    pmm_free_page(page);

    /* Crash recovery: if the journal holds a CRC-valid commit record newer than
     * the superblock (the commit's superblock write was lost), replay it. */
    if (c->txn_log_start) {
        uint64_t jp = pmm_alloc_page();
        if (jp) {
            struct sfs_journal_rec *j = (struct sfs_journal_rec *)(uintptr_t)jp;
            rd_block(c, c->txn_log_start, j);
            if (j->magic == SFS_JOURNAL_MAGIC &&
                j->crc32 == sfs_crc32((const uint8_t *)j + 8, 40) &&
                j->txn_id > c->generation) {
                c->root_btree = j->root_btree;
                c->next_free  = j->next_free;
                c->next_inode = j->next_inode;
                c->generation = j->txn_id - 1;
                sfs_write_super(c);            /* checkpoint the recovered state */
            }
            pmm_free_page(jp);
        }
    }

    *ctx = c;
    return 0;
}

int sfs_format(struct blk_device *bd) {
    if (!bd || bd->capacity_sectors < SFS_SECTORS_PER_BLOCK * 3)
        return -1;
    uint64_t total = bd->capacity_sectors / SFS_SECTORS_PER_BLOCK;
    uint64_t page = pmm_alloc_page();
    if (!page)
        return -1;
    uint8_t *b = (uint8_t *)(uintptr_t)page;

    /* Layout: 0 superblock, 1 root B+tree leaf, 2 root inode, 3 journal;
     * data/metadata allocate from block 4 upward. */
    memset(b, 0, SFS_BLOCK_SIZE);
    struct sfs_superblock *sb = (struct sfs_superblock *)b;
    sb->magic            = SFS_MAGIC;
    sb->version          = SFS_VERSION;
    sb->block_size       = SFS_BLOCK_SIZE;
    sb->total_blocks     = total;
    sb->root_btree       = 1;
    sb->generation       = 1;
    sb->next_free_block  = 4;
    sb->next_inode       = SFS_ROOT_INODE + 1;
    sb->txn_log_start    = 3;
    sb->txn_log_blocks   = 1;
    sb->free_block_count = total > 4 ? total - 4 : 0;
    wr_block_bd(bd, 0, b);

    /* block 1: root B+tree leaf holding the root directory's inode entry */
    memset(b, 0, SFS_BLOCK_SIZE);
    struct sfs_node *root = (struct sfs_node *)b;
    root->flags = SFS_NODE_LEAF;
    root->nkeys = 1;
    root->generation = 1;
    root->u.leaf[0].key = SFS_KEY_INODE | SFS_ROOT_INODE;
    root->u.leaf[0].v.ino.inode_block = 2;
    wr_block_bd(bd, 1, b);

    /* block 2: root directory inode */
    memset(b, 0, SFS_BLOCK_SIZE);
    struct sfs_inode *rin = (struct sfs_inode *)b;
    rin->flags = SFS_I_DIR;
    wr_block_bd(bd, 2, b);

    /* block 3: journal — zeroed (no pending transaction) */
    memset(b, 0, SFS_BLOCK_SIZE);
    wr_block_bd(bd, 3, b);

    pmm_free_page(page);
    return 0;
}

/* ---- snapshots (slice 4h) ------------------------------------------------ */

/* Capture the current tree as an immutable snapshot; returns its id (0 on full).
 * CoW + the non-reclaiming high-water allocator guarantee the captured root and
 * everything reachable from it stay valid (snapshot GC awaits the free-space
 * tree). */
static uint64_t sfs_snapshot(struct sfs_ctx *c) {
    if (c->snapshot_count >= SFS_MAX_SNAPSHOTS)
        return 0;
    uint64_t id = c->generation;
    c->snapshots[c->snapshot_count].id   = id;
    c->snapshots[c->snapshot_count].root = c->root_btree;
    c->snapshot_count++;
    sfs_write_super(c);
    return id;
}

/* Open a file as it existed in snapshot `snap_id` (read-only). */
static int sfs_open_version(struct sfs_ctx *c, const char *path, uint64_t snap_id,
                           struct vfs_file *out) {
    uint64_t sroot = 0;
    for (uint32_t i = 0; i < c->snapshot_count; i++)
        if (c->snapshots[i].id == snap_id) { sroot = c->snapshots[i].root; break; }
    if (!sroot)
        return -1;
    const char *name = skip_slashes(path);
    int len = name_len_of(name);
    if (len <= 0 || len > 255)
        return -1;
    uint64_t dir_key = (SFS_ROOT_INODE << 32) | (uint64_t)sfs_name_hash32(name, len);
    struct sfs_leaf_slot s;
    if (bt_search_root(c, sroot, dir_key, &s) != 0)
        return -1;
    if (s.v.dir.name_len != len)
        return -1;
    for (int i = 0; i < len; i++)
        if (s.v.dir.name[i] != name[i])
            return -1;
    uint64_t ino = s.v.dir.inode_num;
    uint64_t ip = pmm_alloc_page();
    if (!ip) return -1;
    struct sfs_inode *in = (struct sfs_inode *)(uintptr_t)ip;
    if (!inode_block_of_root(c, sroot, ino, in)) { pmm_free_page(ip); return -1; }
    out->size = in->size;
    out->cookie = (uint32_t)ino;
    out->dirent_clus = sroot;          /* read this version through the snapshot root */
    out->dirent_off = 0;
    pmm_free_page(ip);
    return 0;
}

/* Snapshot self-test (slice 4h, destructive). Returns bit0 snapshot-intact,
 * bit1 current-reflects-v2; 3 = passed. */
int sfs_selftest_snapshot(struct blk_device *bd) {
    void *ctx;
    struct sfs_ctx *c;
    int result = 0;

    if (sfs_format(bd) != 0) return -1;
    if (sfs_mount(bd, &ctx) != 0) return -1;
    c = (struct sfs_ctx *)ctx;

    uint64_t a = pmm_alloc_page(), b = pmm_alloc_page();
    if (!a || !b) { sfs_umount(c); return -1; }
    uint8_t *pa = (uint8_t *)(uintptr_t)a, *pb = (uint8_t *)(uintptr_t)b;
    for (int i = 0; i < 4096; i++) pa[i] = (uint8_t)(i * 13 + 1);   /* v1 pattern */

    /* v1: create VER and write 4 KiB of pattern A. */
    struct vfs_file f;
    if (sfs_create(c, "VER", &f) != 0) { sfs_umount(c); return -1; }
    if (sfs_write(c, &f, 0, pa, 4096) != 4096) { sfs_umount(c); return -1; }

    /* snapshot, then grow to v2 by appending a different 4 KiB. */
    uint64_t snap = sfs_snapshot(c);
    struct vfs_file cur;
    if (sfs_open(c, "VER", &cur) != 0) { sfs_umount(c); return -1; }
    for (int i = 0; i < 4096; i++) pb[i] = (uint8_t)(i * 7 + 99);   /* v2 tail */
    sfs_write(c, &cur, 4096, pb, 4096);

    /* read v1 back through the snapshot: must be 4 KiB of pattern A, unchanged. */
    struct vfs_file v1;
    if (sfs_open_version(c, "VER", snap, &v1) == 0 && v1.size == 4096) {
        uint64_t r = pmm_alloc_page();
        if (r) {
            uint8_t *pr = (uint8_t *)(uintptr_t)r;
            int n = sfs_read(c, &v1, 0, pr, 4096);
            if (n == 4096 && memcmp(pr, pa, 4096) == 0)
                result |= 1;                       /* snapshot intact */
            pmm_free_page(r);
        }
    }
    /* current must reflect v2 (size 8192). */
    struct vfs_file v2;
    if (sfs_open(c, "VER", &v2) == 0 && v2.size == 8192)
        result |= 2;

    pmm_free_page(a);
    pmm_free_page(b);
    sfs_umount(c);
    return result;
}

/* End-to-end journal verification with real mount/unmount cycles (slice 4g).
 * Destructive: reformats the device. Returns a 3-bit result (7 == all passed). */
int sfs_selftest_journal(struct blk_device *bd) {
    void *ctx;
    struct sfs_ctx *c;
    uint64_t ino;
    int result = 0;

    /* (1) abort discards: create in a txn, abort, remount -> must be absent. */
    if (sfs_format(bd) != 0) return -1;
    if (sfs_mount(bd, &ctx) != 0) return -1;
    c = (struct sfs_ctx *)ctx;
    sfs_txn_begin(c);
    sfs_do_create(c, SFS_ROOT_INODE, "AAA", 3, &ino);
    sfs_txn_abort(c);
    sfs_umount(c);
    if (sfs_mount(bd, &ctx) != 0) return -1;
    c = (struct sfs_ctx *)ctx;
    if (sfs_do_lookup(c, SFS_ROOT_INODE, "AAA", 3, &ino) != 0)
        result |= 1;                                  /* absent: abort worked */
    sfs_umount(c);

    /* (2) commit persists: create in a txn, commit, remount -> must be present. */
    if (sfs_mount(bd, &ctx) != 0) return -1;
    c = (struct sfs_ctx *)ctx;
    sfs_txn_begin(c);
    sfs_do_create(c, SFS_ROOT_INODE, "BBB", 3, &ino);
    sfs_txn_commit(c);
    sfs_umount(c);
    if (sfs_mount(bd, &ctx) != 0) return -1;
    c = (struct sfs_ctx *)ctx;
    if (sfs_do_lookup(c, SFS_ROOT_INODE, "BBB", 3, &ino) == 0)
        result |= 2;                                  /* present: commit worked */
    sfs_umount(c);

    /* (3) torn-commit replay: write the journal but NOT the superblock (crash
     *     between the two), remount -> recovery must replay the record. */
    if (sfs_mount(bd, &ctx) != 0) return -1;
    c = (struct sfs_ctx *)ctx;
    sfs_txn_begin(c);
    sfs_do_create(c, SFS_ROOT_INODE, "CCC", 3, &ino);
    c->in_txn = 0;
    sfs_journal_write(c);                              /* journal only ... */
    sfs_umount(c);                                     /* ... superblock lost */
    if (sfs_mount(bd, &ctx) != 0) return -1;
    c = (struct sfs_ctx *)ctx;
    if (sfs_do_lookup(c, SFS_ROOT_INODE, "CCC", 3, &ino) == 0)
        result |= 4;                                  /* present via replay */
    sfs_umount(c);

    return result;
}

static const struct vfs_fs_ops sfs_ops = {
    .name       = "sfs",
    .mount      = sfs_mount,
    .open       = sfs_open,
    .create     = sfs_create,
    .read       = sfs_read,
    .write      = sfs_write,
    .unlink     = sfs_unlink,
    .readdir    = sfs_readdir,
    .txn_begin  = sfs_txn_begin,
    .txn_commit = sfs_txn_commit,
    .txn_abort  = sfs_txn_abort,
    .umount     = sfs_umount,
};

void sfs_register(void) {
    vfs_register(&sfs_ops);
}
