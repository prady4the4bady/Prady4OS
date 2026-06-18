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

/* "/HELLO.TXT" -> "HELLO   TXT" (11 bytes, space-padded, upper-cased). */
static void key_83(const char *path, char *key) {
    for (int i = 0; i < 11; i++)
        key[i] = ' ';
    while (*path == '/')
        path++;
    int i = 0;
    while (*path && *path != '.' && i < 8) {
        char c = *path++;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        key[i++] = c;
    }
    while (*path && *path != '.')
        path++;
    if (*path == '.') {
        path++;
        int j = 8;
        while (*path && j < 11) {
            char c = *path++;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            key[j++] = c;
        }
    }
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

static int fat32_readdir(int index, char *name, uint32_t *size) {
    int count = 0;
    uint32_t clus = g_root_clus;
    while (valid_chain(clus)) {
        for (uint32_t s = 0; s < g_spc; s++) {
            uint8_t *sec = rd_data(clus_first_sector(clus) + s);
            for (int e = 0; e < 16; e++) {
                uint8_t *de = sec + e * 32;
                if (de[0] == 0x00) return -1;          /* end of directory */
                if (de[0] == 0xE5) continue;           /* deleted */
                uint8_t attr = de[11];
                if ((attr & 0x0F) == 0x0F) continue;   /* long-name entry */
                if (attr & 0x08) continue;             /* volume label */
                if (count == index) {
                    fmt_83(de, name);
                    if (size) *size = rd32(de, 28);
                    return 0;
                }
                count++;
            }
        }
        clus = fat_next(clus);
    }
    return -1;
}

static int fat32_open(const char *path, struct vfs_file *out) {
    char key[11];
    key_83(path, key);
    uint32_t clus = g_root_clus;
    while (valid_chain(clus)) {
        for (uint32_t s = 0; s < g_spc; s++) {
            uint8_t *sec = rd_data(clus_first_sector(clus) + s);
            for (int e = 0; e < 16; e++) {
                uint8_t *de = sec + e * 32;
                if (de[0] == 0x00) return -1;
                if (de[0] == 0xE5) continue;
                uint8_t attr = de[11];
                if ((attr & 0x0F) == 0x0F || (attr & 0x08)) continue;
                int match = 1;
                for (int i = 0; i < 11; i++)
                    if ((char)de[i] != key[i]) { match = 0; break; }
                if (match) {
                    out->cookie = ((uint32_t)rd16(de, 20) << 16) | rd16(de, 26);
                    out->size = rd32(de, 28);
                    return 0;
                }
            }
        }
        clus = fat_next(clus);
    }
    return -1;
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
