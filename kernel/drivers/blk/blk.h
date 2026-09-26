/* kernel/drivers/blk/blk.h — generic block device interface (Phase 3).
 *
 * Every storage driver (virtio-blk now; NVMe/ATA later) registers a blk_device.
 * Sectors are 512 bytes. Buffers passed to read/write must be physically
 * contiguous and identity-mapped (phys == virtual) for now.
 */
#pragma once
#include <stdint.h>

struct blk_device {
    const char *name;
    uint64_t    capacity_sectors;
    int (*read)(struct blk_device *bd, uint64_t lba, void *buf, uint32_t count);
    int (*write)(struct blk_device *bd, uint64_t lba, const void *buf, uint32_t count);
    void *drv;                 /* driver-private */
};

void               blk_register(struct blk_device *bd);
unsigned           blk_count(void);
struct blk_device *blk_get(unsigned i);

int blk_read(unsigned dev, uint64_t lba, void *buf, uint32_t count);
int blk_write(unsigned dev, uint64_t lba, const void *buf, uint32_t count);

/* DDR-1143 §4.1 — partitions (blk_part.c). */
struct mbr_part {
    uint8_t  index;        /* 0..3: slot in the on-disk table */
    uint8_t  type;
    uint8_t  boot;         /* 0x80 = active */
    uint32_t lba;
    uint32_t count;
};
/* Register a sub-device of `sectors` sectors starting at parent LBA `start`.
 * Returns its registry index, -EINVAL if it would not fit inside the parent,
 * -ENODEV for a bad parent, -ENOSPC if no slot is left. */
int  blk_part_create(unsigned parent, uint64_t start, uint64_t sectors);
/* Usable MBR entries (0..4) into out[], or -EINVAL without the 0x55AA mark. */
int  blk_mbr_parse(const uint8_t *sec0, uint64_t cap, struct mbr_part out[4]);
void blk_mbr_set(uint8_t *sec0, unsigned index, uint8_t type, uint8_t boot,
                 uint32_t lba, uint32_t count);
