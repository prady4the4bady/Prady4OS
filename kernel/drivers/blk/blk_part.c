/* kernel/drivers/blk/blk_part.c — DDR-1143 §4.1: partition sub-devices and the
 * MBR parser the installed-boot root selection reads.
 *
 * A partition is a blk_device whose read/write add a fixed LBA offset and
 * bound every request against the PARTITION end, not the parent's. So SFS
 * mounts a partition unchanged: it sees a device of `sectors` sectors starting
 * at 0, exactly as it sees a whole disk.
 *
 * Bounds are REFUSED, never clamped or wrapped (DDR-1143 §4.1): an out-of-range
 * request returns -EINVAL and nothing reaches the parent. The check is written
 * as a subtraction so that `lba + count` cannot wrap for a huge lba — the same
 * shape ramdisk.c uses, for the same reason.
 *
 * Partitions are REGISTERED devices, because vfs_mount() takes a registry index
 * and sfs_bd_is_registered() (DDR-985) refuses a device it cannot find in the
 * registry. They are appended, so no existing device index moves.
 */
#include "blk.h"
#include "errno.h"
#include "string.h"

#define BLK_PART_MAX 4u

struct blk_part {
    struct blk_device  bd;
    struct blk_device *parent;
    uint64_t           start;           /* first parent LBA of the partition */
};
static struct blk_part g_part[BLK_PART_MAX];
static unsigned        g_part_n;

/* 1 if [lba, lba+count) lies inside [0, cap), computed without overflow. */
static int in_range(uint64_t cap, uint64_t lba, uint64_t count) {
    return lba <= cap && count <= cap - lba;
}

static int part_read(struct blk_device *bd, uint64_t lba, void *buf, uint32_t count) {
    struct blk_part *p = (struct blk_part *)bd->drv;
    if (!in_range(bd->capacity_sectors, lba, count))
        return -EINVAL;
    return p->parent->read(p->parent, p->start + lba, buf, count);
}

static int part_write(struct blk_device *bd, uint64_t lba, const void *buf, uint32_t count) {
    struct blk_part *p = (struct blk_part *)bd->drv;
    if (!in_range(bd->capacity_sectors, lba, count))
        return -EINVAL;
    return p->parent->write(p->parent, p->start + lba, buf, count);
}

/* A partition has no cache of its own: durability is the parent's, so the
 * flush is forwarded. A parent without the op answers -ENOSYS through here
 * rather than being reported as flushed. */
static int part_flush(struct blk_device *bd) {
    struct blk_part *p = (struct blk_part *)bd->drv;
    if (!p->parent->flush)
        return -ENOSYS;
    return p->parent->flush(p->parent);
}

int blk_part_create(unsigned parent, uint64_t start, uint64_t sectors) {
    struct blk_device *pd = blk_get(parent);
    if (!pd || !pd->read || !pd->write)
        return -ENODEV;
    if (sectors == 0 || !in_range(pd->capacity_sectors, start, sectors))
        return -EINVAL;
    if (g_part_n >= BLK_PART_MAX)
        return -ENOSPC;

    struct blk_part *p = &g_part[g_part_n];
    p->parent              = pd;
    p->start               = start;
    p->bd.name             = "part";
    p->bd.capacity_sectors = sectors;
    p->bd.read             = part_read;
    p->bd.write            = part_write;
    p->bd.flush            = part_flush;
    p->bd.drv              = p;

    unsigned before = blk_count();
    blk_register(&p->bd);
    if (blk_count() == before)
        return -ENOSPC;                 /* registry full; slot not consumed */
    g_part_n++;
    return (int)before;
}

/* Parse a classic MBR partition table out of sector 0.
 *
 * Returns the number of USABLE entries written to out[] (0..4), or -EINVAL if
 * the 0x55AA signature is absent. An entry is usable only if its type byte is
 * non-zero, its sector count is non-zero, and it lies wholly inside `cap`
 * sectors — a table claiming space the disk does not have is refused per entry
 * rather than trusted, because root selection (DDR-1143 §4.6) will hand these
 * numbers straight to blk_part_create(). Unusable entries are skipped, and the
 * entry's slot number is kept in `index` so a caller can still say "P2". */
int blk_mbr_parse(const uint8_t *sec0, uint64_t cap, struct mbr_part out[4]) {
    if (sec0[510] != 0x55 || sec0[511] != 0xAA)
        return -EINVAL;
    int n = 0;
    for (unsigned i = 0; i < 4; i++) {
        const uint8_t *e = sec0 + 446 + 16 * i;
        uint8_t  type  = e[4];
        uint32_t lba   = (uint32_t)e[8]  | (uint32_t)e[9]  << 8 |
                         (uint32_t)e[10] << 16 | (uint32_t)e[11] << 24;
        uint32_t count = (uint32_t)e[12] | (uint32_t)e[13] << 8 |
                         (uint32_t)e[14] << 16 | (uint32_t)e[15] << 24;
        if (type == 0 || count == 0 || !in_range(cap, lba, count))
            continue;
        out[n].index = (uint8_t)i;
        out[n].type  = type;
        out[n].boot  = e[0];
        out[n].lba   = lba;
        out[n].count = count;
        n++;
    }
    return n;
}

/* Write one MBR entry into a sector-0 image (CHS fields set to the LBA-only
 * marker 0xFE/0xFFFF, which every firmware that reads LBA fields accepts).
 * Used by the installer and by the smoke-part probe; kept beside the parser so
 * the two cannot disagree about the layout. */
void blk_mbr_set(uint8_t *sec0, unsigned index, uint8_t type, uint8_t boot,
                 uint32_t lba, uint32_t count) {
    uint8_t *e = sec0 + 446 + 16 * index;
    e[0] = boot;
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
    e[4] = type;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    for (unsigned b = 0; b < 4; b++) {
        e[8 + b]  = (uint8_t)(lba   >> (8 * b));
        e[12 + b] = (uint8_t)(count >> (8 * b));
    }
    sec0[510] = 0x55;
    sec0[511] = 0xAA;
}
