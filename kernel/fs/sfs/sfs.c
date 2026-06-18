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

struct sfs_ctx {
    struct blk_device *bd;
    uint64_t total_blocks;
    uint64_t root_btree;
    uint64_t next_free;
    uint64_t next_inode;
    uint64_t generation;
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

/* Publish a transaction: write the superblock with the new root + counters. */
static void sfs_commit(struct sfs_ctx *c) {
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
    sb->free_block_count = (c->total_blocks > c->next_free)
                         ? (c->total_blocks - c->next_free) : 0;
    wr_block(c, 0, sb);
    pmm_free_page(page);
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

/* Exact-key lookup. 0 + *out on hit, -1 if absent. */
static int bt_search(struct sfs_ctx *c, uint64_t key, struct sfs_leaf_slot *out) {
    uint64_t np = pmm_alloc_page();
    if (!np)
        return -1;
    struct sfs_node *n = (struct sfs_node *)(uintptr_t)np;
    uint64_t blk = c->root_btree;
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

/* File data extents are slice 4f. */
static int sfs_read (void *ctx, const struct vfs_file *f, uint64_t off, void *buf, uint32_t len) {
    (void)ctx; (void)f; (void)off; (void)buf; (void)len; return -1;
}
static int sfs_write(void *ctx, struct vfs_file *f, uint64_t off, const void *buf, uint32_t len) {
    (void)ctx; (void)f; (void)off; (void)buf; (void)len; return -1;
}
static int sfs_unlink(void *ctx, const char *path) {
    (void)ctx; (void)path; return -1;
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
    pmm_free_page(page);
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

    /* block 0: superblock (root B+tree=1, root inode=block 2, allocate from 3) */
    memset(b, 0, SFS_BLOCK_SIZE);
    struct sfs_superblock *sb = (struct sfs_superblock *)b;
    sb->magic            = SFS_MAGIC;
    sb->version          = SFS_VERSION;
    sb->block_size       = SFS_BLOCK_SIZE;
    sb->total_blocks     = total;
    sb->root_btree       = 1;
    sb->generation       = 1;
    sb->next_free_block  = 3;
    sb->next_inode       = SFS_ROOT_INODE + 1;
    sb->free_block_count = total > 3 ? total - 3 : 0;
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

    pmm_free_page(page);
    return 0;
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
