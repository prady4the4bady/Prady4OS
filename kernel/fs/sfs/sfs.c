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
#include "errno.h"      /* DDR-891: distinct create failure codes, not bare -1 */
#include "lz4.h"
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
    /* DDR-762-v2: in-memory free-EXTENT-RUN list (within-a-boot reclamation). SFS
     * extents need CONTIGUOUS blocks, so reclamation tracks free RUNS
     * ([start,count]), not individual blocks. A run enters only when
     * snapshot_count==0 (no snapshot can reference it), so reused blocks are never
     * snapshot-referenced. Overflow leaks (bounded). First-fit + split; exact-size
     * reuse is fragmentation-free for uniform files. */
    struct { uint64_t start; uint32_t count; } free_runs[256];
    uint32_t free_run_count;
    uint64_t free_list_blk;            /* DDR-889: on-disk free list block */
};

/* ---- block I/O (one 4 KiB block = 8 sectors; buffers are PMM pages) ------- */
/* Bare device I/O, usable before a ctx exists (mount/format). */
/* DDR-953 instrument — name the caller that reached a freed sfs_ctx.
 *
 * The captured panic (CI 32086772141) was a #PF in rd_block_bd with
 * bd == SFS_MAGIC (0x53465331) and CR2 == bd + 0x10, the bd->read slot.
 * struct sfs_ctx begins with `bd` at offset 0, so c->bd is *(uint64_t*)c:
 * a stale sfs_ctx* was aliasing a buffer that held a superblock.
 *
 * WHY NOT POISON-ON-FREE. The obvious instrument is to poison the ctx in
 * sfs_umount before kfree. It would NOT have caught this crash: bd held
 * SFS_MAGIC, which means the chunk had already been REALLOCATED and
 * overwritten with superblock bytes, so any poison written at free time was
 * overwritten too. Poison only catches a dereference that happens BEFORE
 * reuse — a strict subset, and not the case on record. (KHEAP_DEBUG already
 * provides free-poisoning for that subset.)
 *
 * What the capture is missing is not WHAT was corrupt — that is settled — but
 * WHICH caller held the dead pointer. So the check goes at the dereference and
 * reports its return address; sym_at.py turns that into a function name.
 *
 * The validity test is EXACT, not a heuristic: a live bd is by construction one
 * of the registered block devices, so it is compared against that list rather
 * than against an address range. blk_count() is bounded by VBLK_MAX (4).
 *
 * On detection this prints and RETURNS, letting the natural fault proceed, so
 * the existing register dump is preserved intact and behaviour on a valid
 * pointer is bit-identical. It reports; it does not recover. */
static int sfs_bd_is_registered(const struct blk_device *bd) {
    if ((uint64_t)(uintptr_t)bd < 0x1000ull)
        return 0;
    for (unsigned i = 0; i < blk_count(); i++)
        if ((const struct blk_device *)blk_get(i) == bd)
            return 1;
    return 0;
}

static void sfs_bd_guard(const struct blk_device *bd, uint64_t blk,
                         const char *op, void *ret) {
    if (sfs_bd_is_registered(bd))
        return;
    kputs("[sfs-uaf] STALE CTX op="); kputs(op);
    kputs(" bd=");     kputhex((uint64_t)(uintptr_t)bd);
    kputs(" blk=");    kputdec(blk);
    kputs(" caller="); kputhex((uint64_t)(uintptr_t)ret);
    if ((uint64_t)(uintptr_t)bd == SFS_MAGIC)
        kputs(" note=bd-holds-SFS_MAGIC-ctx-aliases-a-superblock");
    kputs("\r\n");
}

static void rd_block_bd(struct blk_device *bd, uint64_t blk, void *buf) {
    sfs_bd_guard(bd, blk, "read", __builtin_return_address(0));
    bd->read(bd, blk * SFS_SECTORS_PER_BLOCK, buf, SFS_SECTORS_PER_BLOCK);
}
static void wr_block_bd(struct blk_device *bd, uint64_t blk, const void *buf) {
    sfs_bd_guard(bd, blk, "write", __builtin_return_address(0));
    bd->write(bd, blk * SFS_SECTORS_PER_BLOCK, buf, SFS_SECTORS_PER_BLOCK);
}
static void rd_block(struct sfs_ctx *c, uint64_t blk, void *buf) {
    sfs_bd_guard(c->bd, blk, "read-ctx", __builtin_return_address(0));
    rd_block_bd(c->bd, blk, buf);
}
static void wr_block(struct sfs_ctx *c, uint64_t blk, const void *buf) {
    sfs_bd_guard(c->bd, blk, "write-ctx", __builtin_return_address(0));
    wr_block_bd(c->bd, blk, buf);
}

/* DDR-762-v2: reclaim a CONTIGUOUS run [start,count) for reuse — ONLY when no
 * snapshot exists (a snapshot could reference it) and the list has room; else it
 * leaks (bounded) rather than risk a snapshot referencing reused blocks. No
 * coalescing (correctness doesn't need it; exact-size reuse is fragmentation-free). */
static void free_run(struct sfs_ctx *c, uint64_t start, uint32_t count) {
    if (count == 0 || start == 0 || c->snapshot_count != 0)
        return;
    if (c->free_run_count < (uint32_t)(sizeof c->free_runs / sizeof c->free_runs[0])) {
        c->free_runs[c->free_run_count].start = start;
        c->free_runs[c->free_run_count].count = count;
        c->free_run_count++;
    }
}

/* Allocate `n` CONTIGUOUS blocks: first-fit a reclaimed run (splitting it), else
 * bump the high water. The returned start begins a run of exactly `n` blocks. */
static uint64_t alloc_run(struct sfs_ctx *c, uint32_t n) {
    /* EXACT-fit, never split. Splitting a larger run for a smaller request lets
     * single-block (inode/B+tree) allocations nibble a freed extent run before
     * the next extent write can reuse it whole — reuse then fails and next_free
     * bumps anyway. Exact-fit keeps a freed 16-block extent run intact for the
     * next 16-block write (uniform files reuse perfectly); a non-matching size
     * simply bumps (bounded leak) rather than fragment. */
    for (uint32_t i = 0; i < c->free_run_count; i++) {
        if (c->free_runs[i].count == n) {
            uint64_t start = c->free_runs[i].start;
            c->free_runs[i] = c->free_runs[--c->free_run_count];   /* swap-remove */
            return start;
        }
    }
    uint64_t start = c->next_free;
    c->next_free += n;
    return start;
}

/* Single-block allocator (inode blocks, B+tree nodes) — a run of 1. */
static uint64_t alloc_block(struct sfs_ctx *c) {
    return alloc_run(c, 1);
}

/* Smallest buddy order whose allocation holds `bytes`. */
static unsigned order_for(uint32_t bytes) {
    unsigned pages = (bytes + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE;
    unsigned order = 0;
    while ((1u << order) < pages) order++;
    return order;
}

/* Write the superblock from the current in-memory state (the commit point),
 * bumping the generation. */
/* DDR-889: defined below, used by the superblock write above it. */
static void sfs_freelist_save(struct sfs_ctx *c);

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
    /* DDR-889: persist the free list, then record where it went. Saving BEFORE
     * writing the superblock matters — the superblock is the commit point, so a
     * root recorded for a block not yet written would survive a crash pointing
     * at stale bytes. */
    sfs_freelist_save(c);
    sb->free_extent_tree = c->free_list_blk;
    sb->snapshot_count   = c->snapshot_count;
    for (uint32_t i = 0; i < SFS_MAX_SNAPSHOTS; i++)
        sb->snapshots[i] = c->snapshots[i];
    wr_block(c, 0, sb);
    pmm_free_page(page);
}

/* ---- DDR-889 (item 31): the free list ON DISK -----------------------------
 *
 * c->free_runs[] was in-memory only, so every reclaimed run was lost at unmount
 * and the volume leaked blocks across a remount — next_free bumped forever even
 * on a filesystem that was mostly free space. The superblock already reserved
 * `free_extent_tree` as a root pointer for exactly this.
 *
 * ON-DISK SHAPE: one block holding {count, runs[]}. Not a B+tree despite the
 * field name. The in-memory model is a bounded 256-entry array with exact-fit
 * and no coalescing (DDR-762-v2), and a tree would be a second structure with
 * different semantics to keep in step with it. 256 runs x 16 bytes = 4 KiB, so
 * the whole list fits one block by construction — the same bound, persisted.
 *
 * The block is allocated ONCE and reused in place. Allocating a fresh one each
 * sync would consume a block per commit, which is a leak dressed up as an
 * allocation.
 */
#define SFS_FREELIST_MAGIC 0x4C465346u          /* 'SFFL' */

struct sfs_freelist_disk {
    uint32_t magic;
    uint32_t count;
    uint64_t reserved;
    struct { uint64_t start; uint32_t count; uint32_t pad; } runs[254];
};
_Static_assert(sizeof(struct sfs_freelist_disk) <= SFS_BLOCK_SIZE,
               "the free list must fit one block");

static void sfs_freelist_save(struct sfs_ctx *c) {
    /* A snapshot may reference any block, so free_run() already refuses to
     * reclaim while one exists. Persisting an empty list in that state is
     * correct and keeps the on-disk root valid. */
    if (!c->free_list_blk) {
        c->free_list_blk = alloc_block(c);      /* once; reused in place after */
        if (!c->free_list_blk)
            return;
    }
    uint64_t page = pmm_alloc_page();
    if (!page)
        return;
    struct sfs_freelist_disk *fl = (struct sfs_freelist_disk *)(uintptr_t)page;
    memset(fl, 0, SFS_BLOCK_SIZE);
    fl->magic = SFS_FREELIST_MAGIC;
    uint32_t n = c->free_run_count;
    if (n > 254) n = 254;                       /* the block's own bound */
    fl->count = n;
    for (uint32_t i = 0; i < n; i++) {
        fl->runs[i].start = c->free_runs[i].start;
        fl->runs[i].count = c->free_runs[i].count;
    }
    wr_block(c, c->free_list_blk, fl);
    pmm_free_page(page);
}

static void sfs_freelist_load(struct sfs_ctx *c, uint64_t blk) {
    c->free_list_blk  = blk;
    c->free_run_count = 0;
    if (!blk)
        return;
    uint64_t page = pmm_alloc_page();
    if (!page)
        return;
    struct sfs_freelist_disk *fl = (struct sfs_freelist_disk *)(uintptr_t)page;
    rd_block(c, blk, fl);
    /* REJECT a list that does not identify itself. Loading whatever bytes are
     * at that block would hand the allocator arbitrary "free" runs and then
     * write real data over live blocks — silent cross-file corruption. An
     * unrecognised list is dropped: blocks leak, which is bounded and safe. */
    if (fl->magic == SFS_FREELIST_MAGIC && fl->count <= 254) {
        for (uint32_t i = 0; i < fl->count; i++) {
            /* Each run must lie inside the volume and below the high water, or
             * it is not a block this filesystem ever allocated. */
            if (fl->runs[i].start == 0 || fl->runs[i].count == 0)
                continue;
            if (fl->runs[i].start + fl->runs[i].count > c->next_free)
                continue;
            c->free_runs[c->free_run_count].start = fl->runs[i].start;
            c->free_runs[c->free_run_count].count = fl->runs[i].count;
            c->free_run_count++;
        }
    }
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

/* Live SFS contexts (mounted, not yet umounted). Diagnostic only — see the
 * `live=` comment at the mount site. */
static int g_sfs_live;

static void sfs_umount(void *ctx) {
    /* DDR-953 STEP 1-A: record every ctx death so the stale pointer seen by
     * sfs_bd_guard can be matched against the ctx that was freed, and by whom.
     * Paired with the [sfs] mount line below. Diagnostic only. */
    kputs("[sfs] umount ctx="); kputhex((uint64_t)(uintptr_t)ctx);
    kputs(" caller="); kputhex((uint64_t)(uintptr_t)__builtin_return_address(0));
    kputs(" live="); kputdec((uint64_t)__atomic_sub_fetch(&g_sfs_live, 1, __ATOMIC_SEQ_CST));
    kputs("\r\n");
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
    if (s.v.dir.inode_num == 0)                /* DDR-741: tombstone = not found */
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
                         const char *name, int len, int is_dir, uint64_t *out_inode) {
    if (len <= 0 || len > 255)
        return -1;
    uint64_t dir_key = (parent << 32) | (uint64_t)sfs_name_hash32(name, len);
    struct sfs_leaf_slot probe;
    if (bt_search(c, dir_key, &probe) == 0 && probe.v.dir.inode_num != 0)
        return -1;                              /* a LIVE entry exists (DDR-741:
                                                 * a tombstone slot is reusable) */

    uint64_t ino  = c->next_inode++;
    uint64_t iblk = alloc_block(c);

    uint64_t ip = pmm_alloc_page();
    if (!ip) return -1;
    struct sfs_inode *in = (struct sfs_inode *)(uintptr_t)ip;
    memset(in, 0, SFS_BLOCK_SIZE);
    if (is_dir) in->flags = SFS_INO_DIR;        /* DDR-738: directory inode */
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

/* Length of the first path component at `p` (up to the next '/' or NUL). */
static int comp_len(const char *p) {
    int n = 0;
    while (p[n] && p[n] != '/') n++;
    return n;
}

static uint64_t inode_block_of(struct sfs_ctx *c, uint64_t ino, struct sfs_inode *in);

/* DDR-738: is `ino` a directory? The root inode is a directory by definition
 * (it predates any inode block on legacy volumes), so it never reads a block. */
static int sfs_is_dir(struct sfs_ctx *c, uint64_t ino) {
    if (ino == SFS_ROOT_INODE)
        return 1;
    uint64_t ip = pmm_alloc_page();
    if (!ip) return 0;
    struct sfs_inode *in = (struct sfs_inode *)(uintptr_t)ip;
    int isdir = inode_block_of(c, ino, in) ? (int)(in->flags & SFS_INO_DIR) : 0;
    pmm_free_page(ip);
    return isdir;
}

/* DDR-738: walk `path` to the directory that should CONTAIN its final component,
 * resolving intermediates. `*final`/`*final_len` name the last component (a file
 * for open/create). With make_dirs, a missing intermediate is created as a
 * directory inode (mkdir -p); without it, every intermediate must already exist
 * and be a directory. Returns 0 with *parent set, or -1 (missing / not-a-dir /
 * empty final). */
static int sfs_walk(struct sfs_ctx *c, const char *path, int make_dirs,
                    uint64_t *parent, const char **final, int *final_len) {
    uint64_t cur = SFS_ROOT_INODE;
    const char *p = skip_slashes(path);
    for (;;) {
        int len = comp_len(p);
        if (len == 0)
            return -1;                          /* trailing slash / empty name */
        const char *next = skip_slashes(p + len);
        if (*next == 0) {                       /* p is the final component */
            *parent = cur; *final = p; *final_len = len;
            return 0;
        }
        /* intermediate component: must resolve to a directory */
        uint64_t child;
        if (sfs_do_lookup(c, cur, p, len, &child) == 0) {
            if (!sfs_is_dir(c, child))
                return -1;                      /* a file in the middle of a path */
        } else if (make_dirs) {
            if (sfs_do_create(c, cur, p, len, 1 /* dir */, &child) != 0)
                return -1;
        } else {
            return -1;                          /* missing intermediate */
        }
        cur = child;
        p = next;
    }
}

static int sfs_open(void *ctx, const char *path, struct vfs_file *out) {
    struct sfs_ctx *c = (struct sfs_ctx *)ctx;
    uint64_t parent; const char *name; int len;
    if (sfs_walk(c, path, 0 /* no mkdir */, &parent, &name, &len) != 0)
        return -1;
    uint64_t ino;
    if (sfs_do_lookup(c, parent, name, len, &ino) != 0)
        return -1;
    if (sfs_is_dir(c, ino))
        return -1;                              /* open() targets files, not dirs */
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
    uint64_t parent; const char *name; int len;
    /* DDR-891: these two used to be the SAME bare -1, which is what has made
     * `[sfs] churn FAIL op=create iter=0 rc=-1` unactionable for three sessions.
     * The rc travels out through vfs_create's driver passthrough untouched, so
     * DDR-888's -EPERM/-EINVAL split at the VFS layer can never name it — the
     * value is SFS's. Distinguish them here instead. */
    if (sfs_walk(c, path, 1 /* mkdir -p intermediates */, &parent, &name, &len) != 0)
        return -ENOENT;                /* parent path unresolvable/uncreatable */
    uint64_t ino;
    if (sfs_do_create(c, parent, name, len, 0 /* file */, &ino) != 0)
        return -ENOSPC;                /* no free inode/extent (or name clash) */
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
            if ((k & SFS_KEY_TYPE_MASK) == SFS_KEY_DIR && (k >> 32) == parent &&
                n->u.leaf[i].v.dir.inode_num != 0) {   /* DDR-741: skip tombstones */
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

/* DDR-738: resolve `path` to the directory inode it names (root when empty). */
static int sfs_resolve_dir(struct sfs_ctx *c, const char *path, uint64_t *dir) {
    const char *p = skip_slashes(path);
    if (*p == 0) { *dir = SFS_ROOT_INODE; return 0; }   /* "" or "/" -> root */
    uint64_t parent; const char *name; int len;
    if (sfs_walk(c, path, 0, &parent, &name, &len) != 0)
        return -1;
    uint64_t ino;
    if (sfs_do_lookup(c, parent, name, len, &ino) != 0)
        return -1;
    if (!sfs_is_dir(c, ino))
        return -1;                              /* readdir targets directories */
    *dir = ino;
    return 0;
}

static int sfs_readdir(void *ctx, const char *path, int index, char *name, uint32_t *size) {
    struct sfs_ctx *c = (struct sfs_ctx *)ctx;
    uint64_t dir;
    if (sfs_resolve_dir(c, path, &dir) != 0)
        return -1;
    int count = 0;
    int r = sfs_dir_walk(c, c->root_btree, dir, index, &count, name);
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
    int ecount = in->extent_count;

    /* Walk extents by logical offset; materialise each (decompress if needed)
     * into a scratch buffer and copy the overlap with [off,off+len). */
    uint8_t *out = (uint8_t *)buf;
    uint32_t copied = 0;
    uint64_t fpos = 0;
    int rc = (int)copied;
    for (int e = 0; e < ecount && copied < len && fpos < fsize; e++) {
        struct sfs_extent_ref ex = in->inline_extents[e];
        uint32_t llen = ex.logical_len;
        if (fpos + llen > off && fpos < off + len) {     /* overlaps the request */
            unsigned ord = order_for(llen);
            uint64_t buf_p = pmm_alloc_pages(ord);       /* materialised extent  */
            if (!buf_p) { rc = -1; goto done; }
            uint8_t *eb = (uint8_t *)(uintptr_t)buf_p;
            if (ex.flags & SFS_EXT_LZ4) {
                unsigned cord = order_for(ex.block_count * SFS_BLOCK_SIZE);
                uint64_t cbp = pmm_alloc_pages(cord);
                if (!cbp) { pmm_free_pages(buf_p, ord); rc = -1; goto done; }
                uint8_t *cb = (uint8_t *)(uintptr_t)cbp;
                for (uint32_t i = 0; i < ex.block_count; i++)
                    rd_block(c, ex.block_start + i, cb + (uint64_t)i * SFS_BLOCK_SIZE);
                uint32_t dlen = lz4_decompress(cb, ex.comp_len, eb, llen);
                pmm_free_pages(cbp, cord);
                if (dlen != llen) { pmm_free_pages(buf_p, ord); rc = -1; goto done; }
            } else {
                for (uint32_t i = 0; i < ex.block_count; i++)
                    rd_block(c, ex.block_start + i, eb + (uint64_t)i * SFS_BLOCK_SIZE);
            }
            for (uint32_t i = 0; i < llen && copied < len; i++) {
                uint64_t fo = fpos + i;
                if (fo >= off && fo < off + len && fo < fsize)
                    out[copied++] = eb[i];
            }
            pmm_free_pages(buf_p, ord);
        }
        fpos += llen;
    }
    rc = (int)copied;
done:
    pmm_free_page(ip);
    return rc;
}

/* Write data blocks for one extent: store the buffer LZ4-compressed if it saves
 * >25%, else raw. Fills `*ext` and returns 0, or -1 on error. Allocates from the
 * high-water allocator (blocks are contiguous). */
static int write_extent(struct sfs_ctx *c, const uint8_t *data, uint32_t len,
                        struct sfs_extent_ref *ext) {
    const uint8_t *store = data;       /* bytes to actually write to disk */
    uint32_t store_len = len;
    uint32_t comp_len = 0;
    uint32_t flags = 0;
    uint64_t cbp = 0;
    unsigned cord = 0;

    if (len >= 64) {                   /* worth trying to compress */
        cord = order_for(len + len / 16 + SFS_BLOCK_SIZE);
        uint64_t htp = pmm_alloc_page();
        cbp = pmm_alloc_pages(cord);
        if (htp && cbp) {
            memset((void *)(uintptr_t)htp, 0, SFS_BLOCK_SIZE);
            uint32_t cap = (1u << cord) * SFS_BLOCK_SIZE;
            uint32_t clen = lz4_compress(data, len, (uint8_t *)(uintptr_t)cbp, cap,
                                         (uint32_t *)(uintptr_t)htp);
            if (clen != 0 && clen < (len / 4) * 3) {
                store = (const uint8_t *)(uintptr_t)cbp;
                store_len = clen;
                comp_len = clen;
                flags = SFS_EXT_LZ4;
            } else {
                pmm_free_pages(cbp, cord);
                cbp = 0;
            }
        } else {
            if (cbp) { pmm_free_pages(cbp, cord); cbp = 0; }
        }
        if (htp) pmm_free_page(htp);
    }

    uint32_t nblocks = (store_len + SFS_BLOCK_SIZE - 1) / SFS_BLOCK_SIZE;
    uint64_t start = alloc_run(c, nblocks);    /* DDR-762-v2: one CONTIGUOUS run */
    uint64_t db = pmm_alloc_page();
    if (!db) { if (cbp) pmm_free_pages(cbp, cord); return -1; }
    uint8_t *dbuf = (uint8_t *)(uintptr_t)db;
    uint32_t done = 0;
    for (uint32_t i = 0; i < nblocks; i++) {
        uint32_t chunk = (store_len - done < SFS_BLOCK_SIZE) ? (store_len - done) : SFS_BLOCK_SIZE;
        memset(dbuf, 0, SFS_BLOCK_SIZE);
        memcpy(dbuf, store + done, chunk);
        wr_block(c, start + i, dbuf);          /* contiguous: [start, start+nblocks) */
        done += chunk;
    }
    pmm_free_page(db);
    if (cbp) pmm_free_pages(cbp, cord);

    ext->block_start = start;
    ext->block_count = nblocks;
    ext->logical_len = len;
    ext->comp_len = comp_len;
    ext->flags = flags;
    return 0;
}

/* Append/grow write: each call adds one extent (independently compressed), so a
 * compressed file can still be appended. `off` must equal the current file size
 * (mid-file overwrite is a later slice). Up to 4 inline extents. */
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

    struct sfs_extent_ref ext;
    if (write_extent(c, (const uint8_t *)buf, len, &ext) != 0) {
        pmm_free_page(ip);
        return -1;
    }
    in->inline_extents[in->extent_count++] = ext;
    in->size = off + len;
    if (ext.flags & SFS_EXT_LZ4)
        in->flags |= SFS_I_LZ4;

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

/* DDR-866 (item 20) — non-zero ftruncate.
 *
 * SFS stores content as up to 4 append-only extents, so a length change is a
 * READ-THEN-REWRITE rather than an in-place extent edit: an extent may be
 * LZ4-compressed, so its byte range cannot be trimmed without decompressing it
 * anyway.
 *
 * The size bound is ENFORCED, not assumed: past SFS_TRUNC_MAX the request is
 * REFUSED rather than silently satisfied at a different length. A "successful"
 * truncate that produced a length nobody asked for is the worst outcome here,
 * because the caller's next write lands at an offset it did not choose.
 *
 * Growing zero-fills, matching POSIX. The zeros are real bytes: SFS has no
 * sparse representation, and inventing one silently would make st_blocks lie.
 */
#define SFS_TRUNC_MAX (64u * 1024u)          /* 16 pages; refuse beyond */

static int sfs_truncate(void *ctx, struct vfs_file *f, uint64_t len) {
    struct sfs_ctx *c = (struct sfs_ctx *)ctx;
    if (f->dirent_clus != 0)
        return -1;                            /* versioned handle is read-only */
    if (len > SFS_TRUNC_MAX)
        return -1;                            /* refused, never partly applied */

    uint64_t ip = pmm_alloc_page();
    if (!ip) return -1;
    struct sfs_inode *in = (struct sfs_inode *)(uintptr_t)ip;
    if (!inode_block_of(c, f->cookie, in)) { pmm_free_page(ip); return -1; }

    uint64_t oldsz = in->size;
    if (len == oldsz) { pmm_free_page(ip); return 0; }   /* exact no-op */

    /* Materialise the surviving prefix BEFORE touching the inode, so a failure
     * here leaves the file exactly as it was. */
    uint8_t *buf = 0;
    uint64_t keep = (len < oldsz) ? len : oldsz;
    unsigned bord = order_for(SFS_TRUNC_MAX);
    if (len > 0) {
        uint64_t bp = pmm_alloc_pages(bord);
        if (!bp) { pmm_free_page(ip); return -1; }
        buf = (uint8_t *)(uintptr_t)bp;
        memset(buf, 0, SFS_TRUNC_MAX);                    /* the zero-fill tail */
        if (keep > 0) {
            int got = sfs_read(ctx, f, 0, buf, (uint32_t)keep);
            if (got < (int)keep) {
                pmm_free_pages(bp, bord);
                pmm_free_page(ip);
                return -1;
            }
        }
#ifdef FTRUNC_DEBUG
        kputs("[ftrunc] keep="); kputhex(keep);
        kputs(" b0="); kputhex(buf[0]);
        kputs(" b1="); kputhex(buf[1]);
        kputs(" b15="); kputhex(buf[15]);
        kputs("\r\n");
#endif
    }

    in->extent_count = 0;                     /* drop the old extents ... */
    in->size = 0;
    in->flags &= ~(uint32_t)SFS_I_LZ4;
    if (len > 0) {                            /* ... and re-write the content */
        struct sfs_extent_ref ext;
        if (write_extent(c, buf, (uint32_t)len, &ext) != 0) {
            pmm_free_pages((uint64_t)(uintptr_t)buf, bord);
            pmm_free_page(ip);
            return -1;
        }
        in->inline_extents[in->extent_count++] = ext;
        in->size = len;
        if (ext.flags & SFS_EXT_LZ4)
            in->flags |= SFS_I_LZ4;
    }
    if (buf)
        pmm_free_pages((uint64_t)(uintptr_t)buf, bord);

    uint64_t niblk = alloc_block(c);          /* CoW the inode, as sfs_write does */
    wr_block(c, niblk, in);
    pmm_free_page(ip);

    struct sfs_leaf_slot s;
    memset(&s, 0, sizeof s);
    s.key = SFS_KEY_INODE | f->cookie;
    s.v.ino.inode_block = niblk;
    if (bt_insert(c, &s))
        return -1;
    sfs_commit(c);
    f->size = len;
    return 0;
}

/* DDR-741: remove a file or an EMPTY directory by tombstoning its DIR entry
 * (inode_num = 0). No B+tree delete and no block reclamation (bounded leak until
 * reformat). Handles both files and dirs through the single vfs_unlink op. */
static int sfs_unlink(void *ctx, const char *path) {
    struct sfs_ctx *c = (struct sfs_ctx *)ctx;
    uint64_t parent; const char *name; int len;
    if (sfs_walk(c, path, 0, &parent, &name, &len) != 0)
        return -1;
    uint64_t ino;
    if (sfs_do_lookup(c, parent, name, len, &ino) != 0)
        return -1;                              /* absent or already a tombstone */

    /* A directory may only be removed when empty (no LIVE child of `ino`). */
    if (sfs_is_dir(c, ino)) {
        char probe[256]; int cnt = 0;
        if (sfs_dir_walk(c, c->root_btree, ino, 0, &cnt, probe) == 1)
            return -1;                          /* not empty (ENOTEMPTY) */
    }

    /* DDR-762-v2: reclaim the removed inode's blocks (data extents + inode block)
     * as CONTIGUOUS runs. free_run is snapshot-guarded (no-op when a snapshot may
     * reference them). Read the inode before tombstoning; a read failure just
     * skips reclaim (leak, not corruption). */
    {
        uint64_t ip = pmm_alloc_page();
        if (ip) {
            struct sfs_inode *in = (struct sfs_inode *)(uintptr_t)ip;
            uint64_t iblk = inode_block_of(c, ino, in);
            if (iblk) {
                unsigned ne = in->extent_count;
                if (ne > 4) ne = 4;             /* only 4 inline extents exist */
                for (unsigned e = 0; e < ne; e++)
                    free_run(c, in->inline_extents[e].block_start,
                             in->inline_extents[e].block_count);   /* the whole run */
                free_run(c, iblk, 1);           /* the inode block */
            }
            pmm_free_page(ip);
        }
    }

    /* Overwrite the name->inode entry with a tombstone (bt_insert replaces). */
    uint64_t dir_key = (parent << 32) | (uint64_t)sfs_name_hash32(name, len);
    struct sfs_leaf_slot s;
    memset(&s, 0, sizeof s);
    s.key = dir_key;
    s.v.dir.inode_num = 0;                       /* tombstone */
    s.v.dir.name_len = 0;
    if (bt_insert(c, &s))
        return -1;
    sfs_commit(c);
    return 0;
}

/* DDR-956: atomic rename. Same contract as sfs_unlink -- the VFS layer already
 * holds the per-mount sleep-mutex, so this function must NOT take it (mnt_lock
 * is not recursive; re-acquiring on this thread would spin against itself).
 *
 * Atomicity: BOTH key changes go into ONE journal transaction. bt_insert stages
 * the new name->inode entry and the old name's tombstone, and a single
 * sfs_commit flushes them together, so a crash replays both or neither. The
 * inode number is REUSED, never copied -- a copy-then-unlink shape would leave
 * the file duplicated in the window between the halves, and a crash there
 * leaves stale data behind. */
static int sfs_rename(void *ctx, const char *old_path, const char *new_path) {
    struct sfs_ctx *c = (struct sfs_ctx *)ctx;
    uint64_t oparent, nparent;
    const char *oname, *nname;
    int olen, nlen;

    if (sfs_walk(c, old_path, 0, &oparent, &oname, &olen) != 0)
        return -1;                       /* missing source or missing parent */
    if (sfs_walk(c, new_path, 0, &nparent, &nname, &nlen) != 0)
        return -1;                       /* destination parent must exist */

    uint64_t ino;
    if (sfs_do_lookup(c, oparent, oname, olen, &ino) != 0)
        return -1;                       /* absent or already a tombstone */

    uint64_t okey = (oparent << 32) | (uint64_t)sfs_name_hash32(oname, olen);
    uint64_t nkey = (nparent << 32) | (uint64_t)sfs_name_hash32(nname, nlen);
    if (okey == nkey)
        return 0;                        /* same entry: POSIX no-op */

    /* A directory source may only move onto an absent or empty destination,
     * mirroring the emptiness rule sfs_unlink applies before removing one. */
    uint64_t dst_ino;
    if (sfs_do_lookup(c, nparent, nname, nlen, &dst_ino) == 0 &&
        sfs_is_dir(c, dst_ino)) {
        char probe[256]; int cnt = 0;
        if (sfs_dir_walk(c, c->root_btree, dst_ino, 0, &cnt, probe) == 1)
            return -1;                   /* destination directory not empty */
    }

    struct sfs_leaf_slot s;

    /* 1: new name -> the SAME inode (bt_insert replaces any existing entry, so
     *    an occupied destination is overwritten inside this transaction). */
    memset(&s, 0, sizeof s);
    s.key = nkey;
    s.v.dir.inode_num = ino;
    s.v.dir.name_len = (uint8_t)nlen;
    for (int i = 0; i < nlen; i++) s.v.dir.name[i] = nname[i];
    if (bt_insert(c, &s))
        return -1;

    /* 2: old name -> tombstone, exactly as sfs_unlink writes it. */
    memset(&s, 0, sizeof s);
    s.key = okey;
    s.v.dir.inode_num = 0;
    s.v.dir.name_len = 0;
    if (bt_insert(c, &s))
        return -1;

    sfs_commit(c);                       /* ONE commit covers both changes */
    return 0;
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
    c->free_run_count = 0;              /* DDR-762-v2: kmalloc doesn't zero */
    c->free_list_blk  = 0;
    sfs_freelist_load(c, sb->free_extent_tree);   /* DDR-889 (item 31) */
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

    /* DDR-953 STEP 1-A: record every ctx birth (pointer + its bd). */
    kputs("[sfs] mount ctx="); kputhex((uint64_t)(uintptr_t)c);
    kputs(" bd=");             kputhex((uint64_t)(uintptr_t)c->bd);
    kputs(" caller=");         kputhex((uint64_t)(uintptr_t)__builtin_return_address(0));
    /* `live` is the count of SFS contexts mounted and not yet umounted. It is
     * the discriminator for the FSRM family (`FSRM FAIL: created file did not
     * persist`, build_status "A FOURTH intermittent family"): the ring-3 FSRM
     * probe holds the root mount while the sfs.c self-tests mount their OWN
     * context on the SAME block device and write to it. Two contexts carry two
     * independent superblock / free-tree / allocator states, so concurrent
     * writes clobber each other's metadata and an already-created file becomes
     * unfindable on reopen — which is what that probe reports.
     *
     * live >= 2 at the failure means the contexts genuinely coexisted;
     * live == 1 throughout refutes the shared-device reading and points at a
     * ctx-lifetime bug instead (DDR-953). Atomic because mounts happen on
     * several CPUs and a torn count would be worse than no count. */
    kputs(" live=");           kputdec((uint64_t)__atomic_add_fetch(&g_sfs_live, 1, __ATOMIC_SEQ_CST));
    kputs("\r\n");
    *ctx = c;
    return 0;
}

/* DDR-762-v2: read the committed high-water block off the device (see sfs.h). */
/* DDR-889 (item 31): read the PERSISTED free list straight off the device and
 * report how many runs it holds, or -1 if the superblock names no list / the
 * block does not identify itself.
 *
 * This exists because the obvious test — unmount, remount, watch reuse — does
 * not discriminate: vfs_mount() returns the cached mount, so the in-memory
 * runs survive and the measurement is the same either way (grew=9 vs grew=10,
 * measured). Reading the device is a smaller claim, but it is a TRUE one.
 */
long sfs_read_freelist_count(struct blk_device *bd) {
    uint64_t page = pmm_alloc_page();
    if (!page)
        return -1;
    struct sfs_superblock *sb = (struct sfs_superblock *)(uintptr_t)page;
    rd_block_bd(bd, 0, sb);
    if (sb->magic != SFS_MAGIC || sb->free_extent_tree == 0) {
        pmm_free_page(page);
        return -1;
    }
    uint64_t root = sb->free_extent_tree;
    struct sfs_freelist_disk *fl = (struct sfs_freelist_disk *)(uintptr_t)page;
    rd_block_bd(bd, root, fl);
    long n = (fl->magic == SFS_FREELIST_MAGIC && fl->count <= 254)
           ? (long)fl->count : -1;
    pmm_free_page(page);
    return n;
}

uint64_t sfs_read_next_free(struct blk_device *bd) {
    if (!bd)
        return 0;
    uint64_t page = pmm_alloc_page();
    if (!page)
        return 0;
    rd_block_bd(bd, 0, (void *)(uintptr_t)page);
    struct sfs_superblock *sb = (struct sfs_superblock *)(uintptr_t)page;
    uint64_t nf = (sb->magic == SFS_MAGIC) ? sb->next_free_block : 0;
    pmm_free_page(page);
    return nf;
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

/* ---- metadata tags (slice 4i) -------------------------------------------- */
/* Each inode carries a ~4 KiB tag region (agent provenance, confidence, version
 * vectors — populated by Layer 6). Conceptually CAP_FS_SFS_ADMIN for writes. */
static int sfs_set_tag(struct sfs_ctx *c, uint64_t ino, const void *data, uint32_t len) {
    struct sfs_inode probe;
    if (len > sizeof probe.tag)
        return -1;
    uint64_t ip = pmm_alloc_page();
    if (!ip) return -1;
    struct sfs_inode *in = (struct sfs_inode *)(uintptr_t)ip;
    if (!inode_block_of(c, ino, in)) { pmm_free_page(ip); return -1; }
    memset(in->tag, 0, sizeof in->tag);
    memcpy(in->tag, data, len);
    uint64_t niblk = alloc_block(c);
    wr_block(c, niblk, in);
    pmm_free_page(ip);
    struct sfs_leaf_slot s;
    memset(&s, 0, sizeof s);
    s.key = SFS_KEY_INODE | ino;
    s.v.ino.inode_block = niblk;
    if (bt_insert(c, &s)) return -1;
    sfs_commit(c);
    return 0;
}
static int sfs_get_tag(struct sfs_ctx *c, uint64_t ino, void *out, uint32_t len) {
    struct sfs_inode probe;
    if (len > sizeof probe.tag)
        return -1;
    uint64_t ip = pmm_alloc_page();
    if (!ip) return -1;
    struct sfs_inode *in = (struct sfs_inode *)(uintptr_t)ip;
    if (!inode_block_of(c, ino, in)) { pmm_free_page(ip); return -1; }
    memcpy(out, in->tag, len);
    pmm_free_page(ip);
    return 0;
}

/* LZ4 + tag self-test (slice 4i, destructive). 7 = all passed. */
int sfs_selftest_lz4(struct blk_device *bd) {
    void *ctx;
    struct sfs_ctx *c;
    int result = 0;
    if (sfs_format(bd) != 0) return -1;
    if (sfs_mount(bd, &ctx) != 0) return -1;
    c = (struct sfs_ctx *)ctx;

    uint64_t sp = pmm_alloc_pages(5), rp = pmm_alloc_pages(5);   /* 128 KiB each */
    if (!sp || !rp) {
        if (sp) pmm_free_pages(sp, 5);
        if (rp) pmm_free_pages(rp, 5);
        sfs_umount(c);
        return -1;
    }
    uint8_t *s = (uint8_t *)(uintptr_t)sp, *r = (uint8_t *)(uintptr_t)rp;
    static const char pat[7] = { 'P','R','A','D','Y','O','S' };
    for (int i = 0; i < 131072; i++) s[i] = (uint8_t)pat[i % 7];

    struct vfs_file f;
    if (sfs_create(c, "BIGZ", &f) == 0 && sfs_write(c, &f, 0, s, 131072) == 131072) {
        uint64_t ip = pmm_alloc_page();
        if (ip) {
            struct sfs_inode *in = (struct sfs_inode *)(uintptr_t)ip;
            if (inode_block_of(c, f.cookie, in) && (in->flags & SFS_I_LZ4) &&
                in->inline_extents[0].block_count < 32)
                result |= 1;                       /* compressed into <32 blocks */
            pmm_free_page(ip);
        }
        struct vfs_file rf;
        if (sfs_open(c, "BIGZ", &rf) == 0 && rf.size == 131072 &&
            sfs_read(c, &rf, 0, r, 131072) == 131072 && memcmp(s, r, 131072) == 0)
            result |= 2;                           /* readback byte-exact */

        const char tag[24] = "agent-provenance-tag-v1";
        if (sfs_set_tag(c, f.cookie, tag, 24) == 0) {
            uint64_t ino = f.cookie;
            sfs_umount(c);
            c = 0;
            if (sfs_mount(bd, &ctx) == 0) {
                c = (struct sfs_ctx *)ctx;
                char tg[24];
                if (sfs_get_tag(c, ino, tg, 24) == 0 && memcmp(tg, tag, 24) == 0)
                    result |= 4;                   /* tag survived remount */
            }
        }
    }
    pmm_free_pages(sp, 5);
    pmm_free_pages(rp, 5);
    if (c) sfs_umount(c);
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
    sfs_do_create(c, SFS_ROOT_INODE, "AAA", 3, 0, &ino);
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
    sfs_do_create(c, SFS_ROOT_INODE, "BBB", 3, 0, &ino);
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
    sfs_do_create(c, SFS_ROOT_INODE, "CCC", 3, 0, &ino);
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
    .rename     = sfs_rename,        /* DDR-956 */
    .readdir    = sfs_readdir,
    .txn_begin  = sfs_txn_begin,
    .txn_commit = sfs_txn_commit,
    .txn_abort  = sfs_txn_abort,
    .umount     = sfs_umount,
    .truncate   = sfs_truncate,          /* DDR-866 */
};

void sfs_register(void) {
    vfs_register(&sfs_ops);
}
