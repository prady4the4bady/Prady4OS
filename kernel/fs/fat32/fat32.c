/* kernel/fs/fat32/fat32.c — FAT32 driver.
 *
 * 8.3 short names, nested absolute paths. Read in slices 4a/4b; per-mount
 * context + writes in slice 4c. Each mounted volume owns a fat32_ctx (geometry
 * + two scratch sectors), so multiple FAT32 volumes can be mounted at once.
 */
#include "fat32.h"
#include "vfs.h"
#include "blk.h"
#include "pmm.h"
#include "kheap.h"
#include "console.h"
#include "string.h"

#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F            /* RO|HID|SYS|VOL — a long-name fragment */
#define FAT32_EOC      0x0FFFFFF8u     /* >= this in the FAT == end of chain    */

struct fat32_ctx {
    struct blk_device *bd;
    uint16_t bps;                  /* bytes per sector (must be 512)            */
    uint8_t  spc;                  /* sectors per cluster                       */
    uint8_t  num_fats;
    uint32_t fatsz;                /* sectors per FAT                           */
    uint32_t fat_start;            /* first FAT sector                          */
    uint32_t data_start;           /* first data sector (cluster 2)             */
    uint32_t root_clus;
    uint32_t total_clus;           /* count of valid cluster numbers (incl 0,1) */
    uint32_t fsinfo_sec;           /* FSInfo sector LBA, 0 if absent            */
    uint32_t free_hint;            /* next-free-cluster search hint             */
    uint64_t scratch;              /* data/dir sector buffer (one PMM page)     */
    uint64_t fatbuf;               /* FAT sector buffer (one PMM page)          */
    uint64_t auxbuf;               /* FSInfo sector buffer (one PMM page)       */
};

static uint16_t rd16(const uint8_t *p, int o) { return p[o] | ((uint16_t)p[o + 1] << 8); }
static uint32_t rd32(const uint8_t *p, int o) {
    return p[o] | ((uint32_t)p[o + 1] << 8) | ((uint32_t)p[o + 2] << 16) | ((uint32_t)p[o + 3] << 24);
}
static void wr16(uint8_t *p, int o, uint16_t v) { p[o] = v & 0xFF; p[o + 1] = (v >> 8) & 0xFF; }
static void wr32(uint8_t *p, int o, uint32_t v) {
    p[o] = v & 0xFF; p[o + 1] = (v >> 8) & 0xFF; p[o + 2] = (v >> 16) & 0xFF; p[o + 3] = (v >> 24) & 0xFF;
}

static uint8_t *rd_data(struct fat32_ctx *c, uint32_t lba) {
    c->bd->read(c->bd, lba, (void *)(uintptr_t)c->scratch, 1);
    return (uint8_t *)(uintptr_t)c->scratch;
}
static uint8_t *rd_fat(struct fat32_ctx *c, uint32_t lba) {
    c->bd->read(c->bd, lba, (void *)(uintptr_t)c->fatbuf, 1);
    return (uint8_t *)(uintptr_t)c->fatbuf;
}

static uint32_t fat_next(struct fat32_ctx *c, uint32_t clus) {
    uint32_t sec = c->fat_start + (clus * 4) / 512;
    uint32_t off = (clus * 4) % 512;
    return rd32(rd_fat(c, sec), off) & 0x0FFFFFFFu;
}

static uint32_t clus_first_sector(struct fat32_ctx *c, uint32_t clus) {
    return c->data_start + (clus - 2) * c->spc;
}

static int valid_chain(uint32_t c) { return c >= 2 && c < FAT32_EOC; }

/* "HELLO   TXT" (11 bytes) -> "HELLO.TXT". */
static void fmt_83(const uint8_t *de, char *out) {
    int n = 0;
    for (int i = 0; i < 8 && de[i] != ' '; i++)
        out[n++] = (char)de[i];
    if (de[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && de[i] != ' '; i++)
            out[n++] = (char)de[i];
    }
    out[n] = 0;
}

/* One path component (length `len`, no slashes) -> "NAME    EXT" (11 bytes,
 * space-padded, upper-cased). */
static void comp_key(const char *comp, int len, char *key) {
    for (int i = 0; i < 11; i++)
        key[i] = ' ';
    int n = 0, i = 0;
    while (n < len && comp[n] != '.' && i < 8) {
        char c = comp[n++];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        key[i++] = c;
    }
    while (n < len && comp[n] != '.')
        n++;
    if (n < len && comp[n] == '.') {
        n++;
        int j = 8;
        while (n < len && j < 11) {
            char c = comp[n++];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            key[j++] = c;
        }
    }
}

struct dirent_info {
    uint32_t first_clus;
    uint32_t size;
    uint8_t  attr;
    uint32_t ent_clus;             /* physical cluster holding this entry */
    uint16_t ent_off;              /* byte offset of the entry within that cluster */
};

/* Scan the directory whose cluster chain starts at `dir_clus`. Two modes:
 *   key != NULL : find the entry whose 11-byte 8.3 name equals key.
 *   key == NULL : find the `index`-th visible entry (0-based), filling name_out.
 * Fills *out on a hit. Returns 0 on hit, -1 if the directory ends first. */
static int dir_scan(struct fat32_ctx *c, uint32_t dir_clus, const char *key,
                    int index, char *name_out, struct dirent_info *out) {
    int count = 0;
    uint32_t clus = dir_clus;
    while (valid_chain(clus)) {
        for (uint32_t s = 0; s < c->spc; s++) {
            uint8_t *sec = rd_data(c, clus_first_sector(c, clus) + s);
            for (int e = 0; e < 16; e++) {
                uint8_t *de = sec + e * 32;
                if (de[0] == 0x00) return -1;          /* end of directory */
                if (de[0] == 0xE5) continue;           /* deleted */
                uint8_t attr = de[11];
                if ((attr & ATTR_LFN) == ATTR_LFN) continue;   /* long-name */
                if (attr & ATTR_VOLUME_ID) continue;           /* volume label */
                if (key) {
                    int match = 1;
                    for (int i = 0; i < 11; i++)
                        if ((char)de[i] != key[i]) { match = 0; break; }
                    if (!match) continue;
                } else if (count++ != index) {
                    continue;
                }
                if (name_out) fmt_83(de, name_out);
                if (out) {
                    out->first_clus = ((uint32_t)rd16(de, 20) << 16) | rd16(de, 26);
                    out->size     = rd32(de, 28);
                    out->attr     = attr;
                    out->ent_clus = clus;
                    out->ent_off  = (uint16_t)(s * 512 + e * 32);
                }
                return 0;
            }
        }
        clus = fat_next(c, clus);
    }
    return -1;
}

/* Resolve `path` as a directory, descending each component; return its starting
 * cluster, or 0 on error. "/" (or "") resolves to the root directory. */
static uint32_t walk_dir(struct fat32_ctx *c, const char *path) {
    uint32_t clus = c->root_clus;
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        int len = 0;
        while (path[len] && path[len] != '/') len++;
        char key[11];
        comp_key(path, len, key);
        struct dirent_info di;
        if (dir_scan(c, clus, key, 0, 0, &di) != 0) return 0;
        if (!(di.attr & ATTR_DIRECTORY)) return 0;   /* not a directory */
        clus = di.first_clus ? di.first_clus : c->root_clus;
        path += len;
    }
    return clus;
}

static int fat32_mount(struct blk_device *bd, void **ctx_out) {
    struct fat32_ctx *c = (struct fat32_ctx *)kmalloc(sizeof(struct fat32_ctx));
    if (!c)
        return -1;
    memset(c, 0, sizeof(*c));
    c->bd = bd;
    c->scratch = pmm_alloc_page();
    c->fatbuf  = pmm_alloc_page();
    c->auxbuf  = pmm_alloc_page();
    if (!c->scratch || !c->fatbuf || !c->auxbuf) {
        kfree(c);
        return -1;
    }

    uint8_t *b = rd_data(c, 0);
    if (rd16(b, 510) != 0xAA55) { kfree(c); return -1; }
    uint16_t bps     = rd16(b, 11);
    uint32_t fatsz32 = rd32(b, 36);
    if (bps != 512 || fatsz32 == 0) { kfree(c); return -1; }   /* not FAT32 */

    c->bps        = bps;
    c->spc        = b[13];
    c->num_fats   = b[16];
    c->fatsz      = fatsz32;
    uint16_t reserved = rd16(b, 14);
    c->fat_start  = reserved;
    c->data_start = reserved + (uint32_t)c->num_fats * fatsz32;
    c->root_clus  = rd32(b, 44);
    c->fsinfo_sec = rd16(b, 48);
    uint32_t total_sec = rd16(b, 19) ? rd16(b, 19) : rd32(b, 32);
    c->total_clus = c->spc ? ((total_sec - c->data_start) / c->spc) + 2 : 0;
    c->free_hint  = 2;

    *ctx_out = c;
    return 0;
}

/* List the `index`-th entry of the directory named by `path` ("/" = root). */
static int fat32_readdir(void *ctx, const char *path, int index, char *name, uint32_t *size) {
    struct fat32_ctx *c = (struct fat32_ctx *)ctx;
    uint32_t dir = walk_dir(c, path);
    if (!dir)
        return -1;
    struct dirent_info di;
    if (dir_scan(c, dir, 0, index, name, &di) != 0)
        return -1;
    if (size) *size = di.size;
    return 0;
}

/* Open a regular file by absolute path, descending subdirectories. */
static int fat32_open(void *ctx, const char *path, struct vfs_file *out) {
    struct fat32_ctx *c = (struct fat32_ctx *)ctx;
    uint32_t clus = c->root_clus;
    while (*path == '/')
        path++;
    for (;;) {
        int len = 0;
        while (path[len] && path[len] != '/') len++;
        if (len == 0)
            return -1;                      /* empty / trailing-slash component */
        char key[11];
        comp_key(path, len, key);
        struct dirent_info di;
        if (dir_scan(c, clus, key, 0, 0, &di) != 0)
            return -1;
        const char *next = path + len;
        while (*next == '/') next++;
        if (*next == 0) {                   /* final component — must be a file */
            if (di.attr & ATTR_DIRECTORY)
                return -1;
            out->cookie      = di.first_clus;
            out->size        = di.size;
            out->dirent_clus = di.ent_clus;
            out->dirent_off  = di.ent_off;
            return 0;
        }
        if (!(di.attr & ATTR_DIRECTORY))    /* intermediate must be a directory */
            return -1;
        clus = di.first_clus ? di.first_clus : c->root_clus;
        path = next;
    }
}

static int fat32_read(void *ctx, const struct vfs_file *f, uint64_t off, void *buf, uint32_t len) {
    struct fat32_ctx *c = (struct fat32_ctx *)ctx;
    uint8_t *out = (uint8_t *)buf;
    uint32_t copied = 0;
    uint64_t pos = 0;                       /* file offset of the current sector */
    uint32_t clus = f->cookie;
    while (valid_chain(clus) && copied < len) {
        for (uint32_t s = 0; s < c->spc && copied < len; s++) {
            uint8_t *sec = rd_data(c, clus_first_sector(c, clus) + s);
            for (int i = 0; i < 512 && copied < len; i++) {
                uint64_t fo = pos + i;
                if (fo >= off && fo < f->size)
                    out[copied++] = sec[i];
            }
            pos += 512;
        }
        clus = fat_next(c, clus);
    }
    return (int)copied;
}

/* ---------------------------------------------------------------- writes -- */

/* Write a 28-bit FAT entry value into every FAT copy (reserved hi nibble kept). */
static void fat_set(struct fat32_ctx *c, uint32_t clus, uint32_t val) {
    uint32_t off = (clus * 4) % 512;
    uint32_t rel = (clus * 4) / 512;
    for (uint8_t f = 0; f < c->num_fats; f++) {
        uint32_t sec = c->fat_start + (uint32_t)f * c->fatsz + rel;
        uint8_t *fb = rd_fat(c, sec);
        uint32_t nv = (rd32(fb, off) & 0xF0000000u) | (val & 0x0FFFFFFFu);
        wr32(fb, off, nv);
        c->bd->write(c->bd, sec, fb, 1);
    }
}

/* Adjust FSInfo free-count by `delta` and record `next_free` (best effort,
 * only if the FSInfo signatures validate). Uses its own buffer (auxbuf). */
static void fsinfo_adjust(struct fat32_ctx *c, int delta, uint32_t next_free) {
    if (!c->fsinfo_sec)
        return;
    c->bd->read(c->bd, c->fsinfo_sec, (void *)(uintptr_t)c->auxbuf, 1);
    uint8_t *b = (uint8_t *)(uintptr_t)c->auxbuf;
    if (rd32(b, 0) != 0x41615252u || rd32(b, 484) != 0x61417272u)
        return;                                  /* not a valid FSInfo sector */
    uint32_t freec = rd32(b, 488);
    if (freec != 0xFFFFFFFFu)
        wr32(b, 488, (uint32_t)((int64_t)freec + delta));
    wr32(b, 492, next_free);
    c->bd->write(c->bd, c->fsinfo_sec, b, 1);
}

/* Find a free cluster, mark it end-of-chain, and return it (0 if the disk is
 * full). Scans FAT sector by sector from free_hint, then wraps. */
static uint32_t alloc_cluster(struct fat32_ctx *c) {
    uint32_t total = c->total_clus ? c->total_clus : 0x0FFFFFF0u;
    uint32_t start = c->free_hint < 2 ? 2 : c->free_hint;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t from = (pass == 0) ? start : 2;
        uint32_t to   = (pass == 0) ? total : start;
        uint32_t cl = from;
        while (cl < to) {
            uint32_t sec = c->fat_start + (cl * 4) / 512;
            uint32_t off = (cl * 4) % 512;
            uint8_t *fb = rd_fat(c, sec);
            for (; off < 512 && cl < to; off += 4, cl++) {
                if ((rd32(fb, off) & 0x0FFFFFFFu) == 0) {
                    fat_set(c, cl, 0x0FFFFFFFu);     /* clobbers fatbuf — done scanning */
                    c->free_hint = cl + 1;
                    fsinfo_adjust(c, -1, c->free_hint);
                    return cl;
                }
            }
        }
    }
    return 0;
}

/* Free a whole cluster chain back to the FAT + FSInfo. */
static void free_chain(struct fat32_ctx *c, uint32_t clus) {
    while (valid_chain(clus)) {
        uint32_t nx = fat_next(c, clus);
        fat_set(c, clus, 0);
        fsinfo_adjust(c, +1, clus);
        if (clus < c->free_hint) c->free_hint = clus;
        clus = nx;
    }
}

/* Zero every sector of a cluster (used for a freshly allocated directory cluster). */
static void zero_cluster(struct fat32_ctx *c, uint32_t clus) {
    uint8_t *b = (uint8_t *)(uintptr_t)c->scratch;
    memset(b, 0, 512);
    uint32_t base = clus_first_sector(c, clus);
    for (uint32_t s = 0; s < c->spc; s++)
        c->bd->write(c->bd, base + s, b, 1);
}

/* Patch a directory entry's first-cluster + size fields in place. */
static int dirent_update(struct fat32_ctx *c, uint32_t ent_clus, uint16_t ent_off,
                         uint32_t first_clus, uint32_t size) {
    uint32_t sec = clus_first_sector(c, ent_clus) + ent_off / 512;
    uint32_t o   = ent_off % 512;
    uint8_t *b = rd_data(c, sec);
    wr16(b, o + 20, (uint16_t)(first_clus >> 16));
    wr16(b, o + 26, (uint16_t)(first_clus & 0xFFFF));
    wr32(b, o + 28, size);
    return c->bd->write(c->bd, sec, b, 1);
}

/* Resolve the parent directory cluster of `path` and the 8.3 key of its final
 * component. Returns the parent cluster (0 on error); fills key[11]. */
static uint32_t resolve_parent(struct fat32_ctx *c, const char *path, char *key) {
    uint32_t clus = c->root_clus;
    while (*path == '/') path++;
    if (!*path) return 0;
    for (;;) {
        int len = 0;
        while (path[len] && path[len] != '/') len++;
        const char *next = path + len;
        while (*next == '/') next++;
        if (*next == 0) {                        /* final component = leaf name */
            if (len == 0) return 0;
            comp_key(path, len, key);
            return clus;
        }
        char k[11];
        comp_key(path, len, k);
        struct dirent_info di;
        if (dir_scan(c, clus, k, 0, 0, &di) != 0) return 0;
        if (!(di.attr & ATTR_DIRECTORY)) return 0;
        clus = di.first_clus ? di.first_clus : c->root_clus;
        path = next;
    }
}

static void write_new_entry(uint8_t *de, const char *key) {
    memset(de, 0, 32);
    for (int i = 0; i < 11; i++)
        de[i] = (uint8_t)key[i];
    de[11] = ATTR_ARCHIVE;                       /* first cluster + size zero via memset */
}

/* Create an empty regular file at `path` (fails if it already exists). */
static int fat32_create(void *ctx, const char *path, struct vfs_file *out) {
    struct fat32_ctx *c = (struct fat32_ctx *)ctx;
    char key[11];
    uint32_t dir = resolve_parent(c, path, key);
    if (!dir)
        return -1;
    struct dirent_info ex;
    if (dir_scan(c, dir, key, 0, 0, &ex) == 0)
        return -1;                               /* already exists */

    uint32_t clus = dir, last = dir;
    while (valid_chain(clus)) {
        last = clus;
        for (uint32_t s = 0; s < c->spc; s++) {
            uint32_t sec = clus_first_sector(c, clus) + s;
            uint8_t *b = rd_data(c, sec);
            for (int e = 0; e < 16; e++) {
                uint8_t *de = b + e * 32;
                if (de[0] == 0x00 || de[0] == 0xE5) {
                    write_new_entry(de, key);
                    c->bd->write(c->bd, sec, b, 1);
                    out->cookie = 0; out->size = 0;
                    out->dirent_clus = clus;
                    out->dirent_off  = (uint16_t)(s * 512 + e * 32);
                    return 0;
                }
            }
        }
        clus = fat_next(c, clus);
    }
    /* directory full — extend it by one zeroed cluster */
    uint32_t nc = alloc_cluster(c);
    if (!nc)
        return -1;
    zero_cluster(c, nc);
    fat_set(c, last, nc);
    uint32_t sec = clus_first_sector(c, nc);
    uint8_t *b = rd_data(c, sec);
    write_new_entry(b, key);
    c->bd->write(c->bd, sec, b, 1);
    out->cookie = 0; out->size = 0;
    out->dirent_clus = nc; out->dirent_off = 0;
    return 0;
}

/* Write `len` bytes at `off`. Allocates clusters all-or-nothing (rolls back on
 * a short disk so the FS is left unchanged), commits the data, updates the
 * directory entry, then read-back-verifies the written range before success. */
static int fat32_write(void *ctx, struct vfs_file *f, uint64_t off, const void *buf, uint32_t len) {
    struct fat32_ctx *c = (struct fat32_ctx *)ctx;
    if (len == 0)
        return 0;
    uint32_t csize = (uint32_t)c->spc * 512;
    uint64_t end = off + len;
    uint32_t need_total = (uint32_t)((end + csize - 1) / csize);

    /* 1) Measure the existing chain. */
    uint32_t first = f->cookie, have = 0, last = 0, cl = first;
    while (valid_chain(cl)) { have++; last = cl; cl = fat_next(c, cl); }

    /* 2) All-or-nothing allocation of the shortfall. */
    uint32_t need = (need_total > have) ? (need_total - have) : 0;
    uint32_t newhead = 0, newtail = 0;
    for (uint32_t i = 0; i < need; i++) {
        uint32_t nc = alloc_cluster(c);
        if (!nc) {                               /* rollback this op's allocations */
            free_chain(c, newhead);
            return -1;
        }
        if (!newhead) newhead = nc; else fat_set(c, newtail, nc);
        newtail = nc;
    }
    if (need) {
        if (first == 0) { first = newhead; f->cookie = newhead; }
        else            { fat_set(c, last, newhead); }
    }

    /* 3) Commit the data (read-modify-write per sector). */
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t pos = 0;
    cl = first;
    while (valid_chain(cl) && pos < end) {
        for (uint32_t s = 0; s < c->spc; s++) {
            uint64_t ss = pos;
            if (ss + 512 > off && ss < end) {
                uint32_t sec = clus_first_sector(c, cl) + s;
                uint8_t *b = rd_data(c, sec);
                for (int i = 0; i < 512; i++) {
                    uint64_t fo = ss + i;
                    if (fo >= off && fo < end)
                        b[i] = src[fo - off];
                }
                c->bd->write(c->bd, sec, b, 1);
            }
            pos += 512;
        }
        cl = fat_next(c, cl);
    }

    /* 4) Update size + the directory entry. */
    uint64_t newsize = (end > f->size) ? end : f->size;
    f->size = newsize;
    dirent_update(c, f->dirent_clus, f->dirent_off, f->cookie, (uint32_t)newsize);

    /* 5) Read-back verification (mandate): re-read [off,end) and compare. */
    uint32_t verified = 0;
    pos = 0;
    cl = f->cookie;
    while (valid_chain(cl) && pos < end) {
        for (uint32_t s = 0; s < c->spc; s++) {
            uint64_t ss = pos;
            if (ss + 512 > off && ss < end) {
                uint8_t *b = rd_data(c, clus_first_sector(c, cl) + s);
                for (int i = 0; i < 512; i++) {
                    uint64_t fo = ss + i;
                    if (fo >= off && fo < end) {
                        if (b[i] != src[fo - off])
                            return -1;             /* write did not persist */
                        verified++;
                    }
                }
            }
            pos += 512;
        }
        cl = fat_next(c, cl);
    }
    if (verified != len)
        return -1;
    return (int)len;
}

/* Delete a regular file: free its chain and tombstone its directory entry. */
static int fat32_unlink(void *ctx, const char *path) {
    struct fat32_ctx *c = (struct fat32_ctx *)ctx;
    char key[11];
    uint32_t dir = resolve_parent(c, path, key);
    if (!dir)
        return -1;
    struct dirent_info di;
    if (dir_scan(c, dir, key, 0, 0, &di) != 0)
        return -1;
    if (di.attr & ATTR_DIRECTORY)
        return -1;                               /* regular files only */
    if (valid_chain(di.first_clus))
        free_chain(c, di.first_clus);
    uint32_t sec = clus_first_sector(c, di.ent_clus) + di.ent_off / 512;
    uint8_t *b = rd_data(c, sec);
    b[di.ent_off % 512] = 0xE5;
    return c->bd->write(c->bd, sec, b, 1);
}

static const struct vfs_fs_ops fat32_ops = {
    .name    = "fat32",
    .mount   = fat32_mount,
    .open    = fat32_open,
    .create  = fat32_create,
    .read    = fat32_read,
    .write   = fat32_write,
    .unlink  = fat32_unlink,
    .readdir = fat32_readdir,
};

void fat32_register(void) {
    vfs_register(&fat32_ops);
}
