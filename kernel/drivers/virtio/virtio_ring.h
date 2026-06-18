/* kernel/drivers/virtio/virtio_ring.h — split virtqueue layout (virtio 1.0).
 *
 * Three separately-allocated, naturally-aligned structures shared with the
 * device: the descriptor table, the driver-owned "avail" ring, and the
 * device-owned "used" ring. On x86 (TSO) a compiler barrier suffices between
 * filling a ring slot and publishing the index; an MFENCE is used before the
 * MMIO notify. See the virtio 1.1 spec §2.6.
 */
#pragma once
#include <stdint.h>

#define VIRTQ_DESC_F_NEXT   1     /* buffer continues in `next` */
#define VIRTQ_DESC_F_WRITE  2     /* device writes this buffer (else read-only) */

struct virtq_desc {
    uint64_t addr;                /* physical address */
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];             /* ring[queue_size]; (used_event follows) */
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;                 /* index of the head descriptor */
    uint32_t len;                /* bytes written by the device */
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];   /* ring[queue_size]; (avail_event follows) */
} __attribute__((packed));

void virtio_wmb(void);   /* store-store barrier (compiler fence on x86 TSO) */
void virtio_mb(void);    /* full fence (MFENCE) — used before the MMIO notify  */
