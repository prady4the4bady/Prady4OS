/* kernel/drivers/virtio/virtio.h — virtqueue object + device status/feature bits.
 *
 * Transport-agnostic queue management (alloc rings, add descriptor chains,
 * reap the used ring). The PCI transport (virtio_pci) wires a queue to a device.
 */
#pragma once
#include <stdint.h>
#include "virtio_ring.h"

/* device_status bits */
#define VIRTIO_STATUS_ACK          1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_FAILED       128

/* common feature bits */
#define VIRTIO_F_VERSION_1   (1ull << 32)

struct virtq {
    struct virtq_desc  *desc;     /* identity-mapped (phys == virt) */
    struct virtq_avail *avail;
    struct virtq_used  *used;
    uint16_t size;
    uint16_t free_head;           /* head of the free-descriptor list */
    uint16_t num_free;
    uint16_t last_used;           /* last used->idx we have reaped */
    uint16_t notify_off;          /* queue_notify_off from the device */
};

/* One element of a descriptor chain. */
struct virtq_buf {
    uint64_t phys;
    uint32_t len;
    int      write;               /* 1 = device writes (VIRTQ_DESC_F_WRITE) */
};

int  virtq_alloc(struct virtq *vq, uint16_t size);
/* Add an n-element chain to the queue. Returns the head descriptor index, or -1
 * if not enough free descriptors. Does NOT publish (call virtq_publish). */
int  virtq_add(struct virtq *vq, const struct virtq_buf *bufs, int n);
/* Publish a previously-added chain head into the avail ring. */
void virtq_publish(struct virtq *vq, int head);
/* Reap one completed chain: returns its head index (and bytes used), or -1. */
int  virtq_pop_used(struct virtq *vq, uint32_t *len_out);
/* Return a descriptor chain to the free list. */
void virtq_free_chain(struct virtq *vq, int head);
