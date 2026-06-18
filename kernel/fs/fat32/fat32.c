/* kernel/fs/fat32/fat32.c — read-only FAT32 (8.3 names, root directory).
 *
 * Enough to mount a FAT32 volume on a block device, list the root directory,
 * and read regular files by short name. Long names, subdirectories, and writes
 * are later work. Single-threaded FS access (two scratch sectors).
 */
#include "fat32.h"
#include "vfs.h"
#include "blk.h"
#include "pmm.h"
#include "console.h"

static struct blk_device *g_bd;
static uint16_t g_bps;          /* bytes per sector */
static uint8_t  g_spc;          /* sectors per cluster */
static uint32_t g_fat_start;    /* first FAT sector */
static uint32_t g_data_start;   /* first data sector (cluster 2) */
static uint32_t g_root_clus;
static uint64_t g_scratch;      /* data/dir sector buffer */
static uint64_t g_fatbuf;       /* FAT sector buffer */

static uint16_t rd16(const uint8_t *p, int o) { return p[o] | ((uint16_t)p[o + 1] << 8); }
static uint32_t rd32(const uint8_t *p, int o) {
    return p[o] | ((uint32_t)p[o + 1] << 8) | ((uint32_t)p[o + 2] << 16) | ((uint32_t)p[o + 3] << 24);
}

static uint8_t *rd_data(uint32_t lba) {
    g_bd->read(g_bd, lba, (void *)(uintptr_t)g_scratch, 1);
    return (uint8_t *)(uintptr_t)g_scratch;
}
static uint8_t *rd_fat(uint32_t lba) {
    g_bd->read(g_bd, lba, (void *)(uintptr_t)g_fatbuf, 1);
    return (uint8_t *)(uintptr_t)g_fatbuf;
}

static uint32_t fat_next(uint32_t clus) {
    uint32_t fat_sec = g_fat_start + (clus * 4) / 512;
    uint32_t off = (clus * 4) % 512;
    return rd32(rd_fat(fat_sec), off) & 0x0FFFFFFF;
}

static uint32_t clus_first_sector(uint32_t clus) {
    return g_data_start + (clus - 2) * g_spc;
}

static int valid_chain(uint32_t c) { return c >= 2 && c < 0x0FFFFFF8; }

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

#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_LFN       0x0F            /* RO|HID|SYS|VOL — a long-name fragment */

struct dirent_info {
    uint32_t first_clus;
    uint32_t size;
    uint8_t  attr;
};

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

/* Scan the directory whose cluster chain starts at `dir_clus`. Two modes:
 *   key != NULL : find the entry whose 11-byte 8.3 name equals key.
 *   key == NULL : find the `index`-th visible entry (0-based), filling name_out.
 * Fills *out (first cluster / size / attr) on a hit. Returns 0 on hit, -1 if the
 * directory ends or the chain is exhausted first. */
static int dir_scan(uint32_t dir_clus, const char *key, int index,
                    char *name_out, struct dirent_info *out) {
    int count = 0;
    uint32_t clus = dir_clus;
    while (valid_chain(clus)) {
        for (uint32_t s = 0; s < g_spc; s++) {
            uint8_t *sec = rd_data(clus_first_sector(clus) + s);
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
                    out->size  = rd32(de, 28);
                    out->attr  = attr;
                }
                return 0;
            }
        }
        clus = fat_next(clus);
    }
    return -1;
}

/* Resolve `path` as a directory, descending each component; return its starting
 * cluster, or 0 on error. "/" (or "") resolves to the root directory. A first
 * cluster of 0 in a "." /".." entry means the root on FAT32. */
static uint32_t walk_dir(const char *path) {
    uint32_t clus = g_root_clus;
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        int len = 0;
        while (path[len] && path[len] != '/') len++;
        char key[11];
        comp_key(path, len, key);
        struct dirent_info di;
        if (dir_scan(clus, key, 0, 0, &di) != 0) return 0;
        if (!(di.attr & ATTR_DIRECTORY)) return 0;   /* not a directory */
        clus = di.first_clus ? di.first_clus : g_root_clus;
        path += len;
    }
    return clus;
}

static int fat32_mount(struct blk_device *bd) {
    if (!g_scratch) g_scratch = pmm_alloc_page();
    if (!g_fatbuf)  g_fatbuf  = pmm_alloc_page();
    if (!g_scratch || !g_fatbuf)
        return -1;
    g_bd = bd;

    uint8_t *b = rd_data(0);
    if (rd16(b, 510) != 0xAA55)        return -1;
    uint16_t bps = rd16(b, 11);
    uint32_t fatsz32 = rd32(b, 36);
    if (bps != 512 || fatsz32 == 0)    return -1;   /* not a FAT32 volume */

    g_bps = bps;
    g_spc = b[13];
    uint16_t reserved = rd16(b, 14);
    uint8_t  num_fats = b[16];
    g_fat_start  = reserved;
    g_data_start = reserved + (uint32_t)num_fats * fatsz32;
    g_root_clus  = rd32(b, 44);
    return 0;
}

/* List the `index`-th entry of the directory named by `path` ("/" = root). */
static int fat32_readdir(const char *path, int index, char *name, uint32_t *size) {
    uint32_t dir = walk_dir(path);
    if (!dir)
        return -1;
    struct dirent_info di;
    if (dir_scan(dir, 0, index, name, &di) != 0)
        return -1;
    if (size) *size = di.size;
    return 0;
}

/* Open a regular file by absolute path, descending subdirectories. */
static int fat32_open(const char *path, struct vfs_file *out) {
    uint32_t clus = g_root_clus;
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
        if (dir_scan(clus, key, 0, 0, &di) != 0)
            return -1;
        const char *next = path + len;
        while (*next == '/') next++;
        if (*next == 0) {                   /* final component — must be a file */
            if (di.attr & ATTR_DIRECTORY)
                return -1;
            out->cookie = di.first_clus;
            out->size   = di.size;
            return 0;
        }
        if (!(di.attr & ATTR_DIRECTORY))    /* intermediate must be a directory */
            return -1;
        clus = di.first_clus ? di.first_clus : g_root_clus;
        path = next;
    }
}

static int fat32_read(const struct vfs_file *f, uint64_t off, void *buf, uint32_t len) {
    uint8_t *out = (uint8_t *)buf;
    uint32_t copied = 0;
    uint64_t pos = 0;                       /* file offset of the current sector */
    uint32_t clus = f->cookie;
    while (valid_chain(clus) && copied < len) {
        for (uint32_t s = 0; s < g_spc && copied < len; s++) {
            uint8_t *sec = rd_data(clus_first_sector(clus) + s);
            for (int i = 0; i < 512 && copied < len; i++) {
                uint64_t fo = pos + i;
                if (fo >= off && fo < f->size)
                    out[copied++] = sec[i];
            }
            pos += 512;
        }
        clus = fat_next(clus);
    }
    return (int)copied;
}

static const struct vfs_fs_ops fat32_ops = {
    .name = "fat32",
    .mount = fat32_mount,
    .open = fat32_open,
    .read = fat32_read,
    .readdir = fat32_readdir,
};

void fat32_register(void) {
    vfs_register(&fat32_ops);
}
