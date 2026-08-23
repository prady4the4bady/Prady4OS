/* kernel/drivers/blk/ramdisk.c — DDR-972: a memory-backed block device.
 *
 * Exists for one configuration: the ISO. DDR-971 measured a booted ISO reaching
 * NEXUS KERNEL OK and then idling forever at rqdepth=1 curpid=0, because the
 * image carries no root filesystem and the kernel cannot read the ATAPI CD it
 * booted from once the loader has handed off. A control arm (same kernel.bin,
 * normal 3-disk boot) reached PRISM_READY with 26 ELF loads, so the kernel was
 * never the problem — only the absence of any block device was.
 *
 * The backing store is one physically contiguous PMM allocation. blk.h requires
 * buffers be contiguous and identity-mapped, which a single buddy allocation
 * satisfies; a page-list ramdisk would not.
 *
 * NOT persistent, by construction. A live ISO's root is volatile and anything
 * written is gone at power-off (DDR-972 §4).
 */
#include "blk.h"
#include "pmm.h"
#include "string.h"
#include "console.h"

/* Order 10 = 1024 frames = 4 MiB. At the point this runs pmm reports ~28,171
 * free frames (~110 MiB), so the root costs 3.6% of free memory. */
#define SECTOR          512u
#define RAMDISK_MAX     3u                /* the ISO topology below */

struct rd {
    struct blk_device bd;
    uint64_t          base;               /* physical == virtual (identity) */
    uint64_t          bytes;
};
static struct rd g_rd[RAMDISK_MAX];
static unsigned  g_rd_n;

static int rd_read(struct blk_device *bd, uint64_t lba, void *buf, uint32_t count) {
    struct rd *r = (struct rd *)bd->drv;
    /* Overflow-safe: `lba + count` wraps for a large lba (lba=UINT64_MAX,
     * count=1 sums to 0 and would pass), so bound lba first and then express
     * the remaining room as a subtraction that cannot wrap. */
    if (lba > bd->capacity_sectors || count > bd->capacity_sectors - lba)
        return -1;
    memcpy(buf, (const void *)(uintptr_t)(r->base + lba * SECTOR), count * SECTOR);
    return 0;
}

static int rd_write(struct blk_device *bd, uint64_t lba, const void *buf, uint32_t count) {
    struct rd *r = (struct rd *)bd->drv;
    /* Overflow-safe: `lba + count` wraps for a large lba (lba=UINT64_MAX,
     * count=1 sums to 0 and would pass), so bound lba first and then express
     * the remaining room as a subtraction that cannot wrap. */
    if (lba > bd->capacity_sectors || count > bd->capacity_sectors - lba)
        return -1;
    memcpy((void *)(uintptr_t)(r->base + lba * SECTOR), buf, count * SECTOR);
    return 0;
}

/* Allocate, ZERO and register. Returns the new device index, or -1.
 *
 * Zeroing is mandatory, not tidiness: vfs_mount probes for FAT32/SFS/ext4
 * signatures, and unzeroed PMM pages can carry a stale pattern that probes as a
 * CORRUPT filesystem rather than a blank one — which would fail the mount
 * instead of falling through to the caller's format.
 */
int ramdisk_init(unsigned order) {
    if (g_rd_n >= RAMDISK_MAX)
        return -1;
    struct rd *r = &g_rd[g_rd_n];

    r->base = pmm_alloc_pages(order);
    if (!r->base) {
        kputs("[ramdisk] FAILED: no contiguous allocation at order ");
        kputdec((uint64_t)order);
        kputs("\r\n");
        return -1;
    }
    r->bytes = (uint64_t)(1u << order) * 4096u;
    memset((void *)(uintptr_t)r->base, 0, (unsigned long)r->bytes);

    r->bd.name             = "ramdisk";
    r->bd.capacity_sectors = r->bytes / SECTOR;
    r->bd.read             = rd_read;
    r->bd.write            = rd_write;
    r->bd.drv              = r;

    unsigned before = blk_count();
    blk_register(&r->bd);
    if (blk_count() == before) {
        kputs("[ramdisk] FAILED: blk registry full\r\n");
        pmm_free_pages(r->base, order);
        r->base = 0;
        return -1;
    }
    g_rd_n++;
    kputs("[ramdisk] blk");
    kputdec((uint64_t)before);
    kputs(" = ");
    kputdec(r->bytes / 1024u);
    kputs(" KiB\r\n");
    return (int)before;
}
