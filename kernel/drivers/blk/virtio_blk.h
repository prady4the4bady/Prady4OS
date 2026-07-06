/* kernel/drivers/blk/virtio_blk.h — virtio-blk driver (Phase 3). */
#pragma once
#include <stdint.h>

/* Attach the virtio-blk device at the given PCI address and register it as a
 * generic block device. Safe to ignore the return; logs on failure. */
void virtio_blk_init(uint8_t bus, uint8_t dev, uint8_t func);

/* DDR-714C3 proof: 1 if any disk's MSI-X completion ran on a non-BSP CPU. */
int virtio_blk_completed_on_ap(void);
