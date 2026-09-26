/* kernel/drivers/blk/virtio_blk.h — virtio-blk driver (Phase 3). */
#pragma once
#include <stdint.h>

/* Attach the virtio-blk device at the given PCI address and register it as a
 * generic block device. Safe to ignore the return; logs on failure. */
void virtio_blk_init(uint8_t bus, uint8_t dev, uint8_t func);

/* DDR-714C3 proof: 1 if any disk's MSI-X completion ran on a non-BSP CPU. */
int virtio_blk_completed_on_ap(void);

/* DDR-1138: print one [vblkown] line per unit naming who holds compl_lock.
 * Called only from the [apfreeze] relay, after lock_stat_dump(). Reads
 * without taking the lock. */
void virtio_blk_dump_owners(void);

/* DDR-1143 §10.2: unit 0's negotiated VIRTIO_BLK_F_FLUSH (-1 if no unit), and
 * the flushes issued / completed with status 0 across all units. */
void virtio_blk_flush_stats(int *neg0, uint32_t *issued, uint32_t *ok);
