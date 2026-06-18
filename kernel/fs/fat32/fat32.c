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
};

static uint16_t rd16(const uint8_t *p, int o) { return p[o] | ((uint16_t)p[o + 1] << 8); }
static uint32_t rd32(const uint8_t *p, int o) {
    return p[o] | ((uint32_t)p[o + 1] << 8) | ((uint32_t)p[o + 2] << 16) | ((uint32_t)p[o + 3] << 24);
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
    if (!c->scratch || !c->fatbuf) {
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

static const struct vfs_fs_ops fat32_ops = {
    .name    = "fat32",
    .mount   = fat32_mount,
    .open    = fat32_open,
    .create  = 0,                  /* slice 4c */
    .read    = fat32_read,
    .write   = 0,                  /* slice 4c */
    .unlink  = 0,                  /* slice 4c */
    .readdir = fat32_readdir,
};

void fat32_register(void) {
    vfs_register(&fat32_ops);
}
