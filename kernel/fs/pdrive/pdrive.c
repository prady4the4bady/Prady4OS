/* kernel/fs/pdrive/pdrive.c — PRADYOS Drive (Group 7 item 40, DDR-890).
 *
 * A RAM-backed VFS filesystem for the agent workspace. It implements the same
 * `struct vfs_fs_ops` every disk filesystem does, so an agent rooted here uses
 * open/create/read/write/unlink/readdir/truncate unchanged — the workspace is a
 * mount, not a special case threaded through the syscall layer.
 *
 * ---------------------------------------------------------------------------
 * WHY IT IS BOUNDED, AND WHY THAT IS THE INTERESTING PART
 *
 * This is memory an AGENT controls the size of. An unbounded RAM filesystem is
 * a denial-of-service primitive handed to the least trusted code in the system:
 * an agent that writes in a loop consumes every free frame and the PMM starves
 * the kernel, not the agent.
 *
 * So both dimensions are capped — PDRIVE_MAX_FILES and PDRIVE_MAX_BYTES — and
 * exceeding either is REFUSED with an error the caller can see. Silently
 * dropping the write, or truncating it, would report success for data that is
 * not there, which is worse than refusing: the agent proceeds believing its
 * workspace holds something it does not.
 */
#include "vfs.h"
#include "pmm.h"
#include "kheap.h"
#include "string.h"
#include "console.h"

#define PDRIVE_MAX_FILES  32
#define PDRIVE_MAX_BYTES  (1u * 1024 * 1024)   /* 1 MiB total, all files */
#define PDRIVE_NAME_MAX   64

struct pd_file {
    char     name[PDRIVE_NAME_MAX];
    uint8_t *data;
    uint32_t size;
    uint32_t cap;
    int      used;
};

struct pd_ctx {
    struct pd_file f[PDRIVE_MAX_FILES];
    uint32_t       total_bytes;
};

/* The kernel's string.h has no strcmp — comparing two NUL-terminated names is
 * the only string operation these paths need, so it is local rather than an
 * addition to a shared header for one caller. */
static int name_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

/* Paths arrive as "/NAME". Leading slashes are skipped so a caller may pass
 * either form; an empty remainder is not a file. */
static const char *basename_of(const char *path) {
    while (*path == '/')
        path++;
    return path;
}

static struct pd_file *find(struct pd_ctx *c, const char *path) {
    const char *n = basename_of(path);
    if (!*n)
        return 0;
    for (int i = 0; i < PDRIVE_MAX_FILES; i++)
        if (c->f[i].used && name_eq(c->f[i].name, n))
            return &c->f[i];
    return 0;
}

static int pd_mount(struct blk_device *bd, void **ctx) {
    /* A virtual filesystem must never claim a DISK. vfs_mount() probes every
     * registered driver against a block device in turn, so a pdrive that
     * accepted a non-NULL bd would swallow the first disk offered to it and
     * whichever real filesystem was on it would never be tried. */
    if (bd)
        return -1;
    struct pd_ctx *c = (struct pd_ctx *)kmalloc(sizeof(struct pd_ctx));
    if (!c)
        return -1;
    memset(c, 0, sizeof *c);        /* kmalloc does not zero */
    *ctx = c;
    return 0;
}

static int pd_open(void *ctx, const char *path, struct vfs_file *out) {
    struct pd_file *f = find((struct pd_ctx *)ctx, path);
    if (!f)
        return -1;
    out->size   = f->size;
    out->cookie = (uint32_t)(f - ((struct pd_ctx *)ctx)->f);
    return 0;
}

static int pd_create(void *ctx, const char *path, struct vfs_file *out) {
    struct pd_ctx *c = (struct pd_ctx *)ctx;
    const char *n = basename_of(path);
    if (!*n || strlen(n) >= PDRIVE_NAME_MAX)
        return -1;
    struct pd_file *f = find(c, path);
    if (f) {                              /* truncate an existing file */
        f->size = 0;
        out->size = 0;
        out->cookie = (uint32_t)(f - c->f);
        return 0;
    }
    for (int i = 0; i < PDRIVE_MAX_FILES; i++) {
        if (c->f[i].used)
            continue;
        memset(&c->f[i], 0, sizeof c->f[i]);
        unsigned k = 0;
        while (n[k] && k < PDRIVE_NAME_MAX - 1) { c->f[i].name[k] = n[k]; k++; }
        c->f[i].name[k] = 0;
        c->f[i].used = 1;
        out->size = 0;
        out->cookie = (uint32_t)i;
        return 0;
    }
    /* Table full. REFUSED, not silently reused: evicting someone else's file to
     * make room would lose an agent's data without telling anyone. */
    return -1;
}

static int pd_read(void *ctx, const struct vfs_file *vf, uint64_t off,
                   void *buf, uint32_t len) {
    struct pd_ctx *c = (struct pd_ctx *)ctx;
    if (vf->cookie >= PDRIVE_MAX_FILES)
        return -1;
    struct pd_file *f = &c->f[vf->cookie];
    if (!f->used || off > f->size)
        return -1;
    uint32_t avail = f->size - (uint32_t)off;
    if (len > avail)
        len = avail;                       /* short read is correct at EOF */
    if (len)
        memcpy(buf, f->data + off, len);
    return (int)len;
}

static int pd_write(void *ctx, struct vfs_file *vf, uint64_t off,
                    const void *buf, uint32_t len) {
    struct pd_ctx *c = (struct pd_ctx *)ctx;
    if (vf->cookie >= PDRIVE_MAX_FILES)
        return -1;
    struct pd_file *f = &c->f[vf->cookie];
    if (!f->used)
        return -1;

    uint64_t end = off + len;
    if (end > PDRIVE_MAX_BYTES)
        return -1;                         /* one file cannot exceed the volume */

    if (end > f->cap) {
        /* Growing: charge the DELTA against the volume budget before allocating,
         * so the cap is enforced on what the workspace will hold rather than on
         * what it held a moment ago. */
        uint32_t newcap = (uint32_t)end;
        uint32_t delta  = newcap - f->cap;
        if (c->total_bytes + delta > PDRIVE_MAX_BYTES)
            return -1;                     /* volume full — refused, not truncated */

        unsigned order = 0;
        while ((4096u << order) < newcap && order < 9)
            order++;
        uint64_t page = pmm_alloc_pages(order);
        if (!page)
            return -1;
        uint8_t *nd = (uint8_t *)(uintptr_t)page;
        memset(nd, 0, 4096u << order);
        if (f->data && f->size)
            memcpy(nd, f->data, f->size);
        if (f->data) {
            unsigned oldorder = 0;
            while ((4096u << oldorder) < f->cap && oldorder < 9)
                oldorder++;
            pmm_free_pages((uint64_t)(uintptr_t)f->data, oldorder);
        }
        f->data = nd;
        c->total_bytes += delta;
        f->cap = 4096u << order;
    }
    memcpy(f->data + off, buf, len);
    if (end > f->size)
        f->size = (uint32_t)end;
    vf->size = f->size;
    return (int)len;
}

static int pd_unlink(void *ctx, const char *path) {
    struct pd_ctx *c = (struct pd_ctx *)ctx;
    struct pd_file *f = find(c, path);
    if (!f)
        return -1;
    if (f->data) {
        unsigned order = 0;
        while ((4096u << order) < f->cap && order < 9)
            order++;
        pmm_free_pages((uint64_t)(uintptr_t)f->data, order);
        c->total_bytes -= (f->cap < c->total_bytes) ? f->cap : c->total_bytes;
    }
    memset(f, 0, sizeof *f);
    return 0;
}

static int pd_readdir(void *ctx, const char *path, int index,
                      char *name, uint32_t *size) {
    struct pd_ctx *c = (struct pd_ctx *)ctx;
    (void)path;                            /* flat namespace: one directory */
    int seen = 0;
    for (int i = 0; i < PDRIVE_MAX_FILES; i++) {
        if (!c->f[i].used)
            continue;
        if (seen == index) {
            unsigned k = 0;
            while (c->f[i].name[k]) { name[k] = c->f[i].name[k]; k++; }
            name[k] = 0;
            if (size)
                *size = c->f[i].size;
            return 0;
        }
        seen++;
    }
    return -1;                             /* end of directory */
}

static int pd_truncate(void *ctx, struct vfs_file *vf, uint64_t len) {
    struct pd_ctx *c = (struct pd_ctx *)ctx;
    if (vf->cookie >= PDRIVE_MAX_FILES)
        return -1;
    struct pd_file *f = &c->f[vf->cookie];
    if (!f->used || len > PDRIVE_MAX_BYTES)
        return -1;
    if (len > f->cap)
        return -1;                         /* grow-by-truncate not supported */
    if (len > f->size)
        memset(f->data + f->size, 0, (uint32_t)len - f->size);
    f->size = (uint32_t)len;
    vf->size = f->size;
    return 0;
}

static const struct vfs_fs_ops pdrive_ops = {
    .name     = "pdrive",
    .mount    = pd_mount,
    .open     = pd_open,
    .create   = pd_create,
    .read     = pd_read,
    .write    = pd_write,
    .unlink   = pd_unlink,
    .readdir  = pd_readdir,
    .truncate = pd_truncate,
};

void pdrive_register(void) {
    vfs_register(&pdrive_ops);
}
