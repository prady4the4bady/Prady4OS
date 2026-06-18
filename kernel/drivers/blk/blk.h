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
