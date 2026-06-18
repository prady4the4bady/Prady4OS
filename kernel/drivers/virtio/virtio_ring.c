/* kernel/drivers/virtio/virtio_ring.c — split-virtqueue memory barriers.
 *
 * On x86 (TSO) stores are not reordered with stores, so a compiler barrier is
 * enough between filling a ring slot and publishing the index. An explicit
 * MFENCE is used before the device notify (MMIO write) to order prior ring
 * writes ahead of the notification.
 */
#include "virtio_ring.h"

void virtio_wmb(void) {
    __asm__ volatile("" ::: "memory");
}

void virtio_mb(void) {
    __asm__ volatile("mfence" ::: "memory");
}
