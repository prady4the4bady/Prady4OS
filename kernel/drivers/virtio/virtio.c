/* kernel/drivers/virtio/virtio.c — virtqueue management. */
#include "virtio.h"
#include "pmm.h"
#include "string.h"

/* Rings are allocated as whole PMM pages (page-aligned, identity-mapped, so the
 * physical address equals the pointer). One page each is enough for queue sizes
 * up to 256 (desc 4 KiB, avail/used < 4 KiB). */
int virtq_alloc(struct virtq *vq, uint16_t size) {
    uint64_t d = pmm_alloc_page();
    uint64_t a = pmm_alloc_page();
    uint64_t u = pmm_alloc_page();
    if (!d || !a || !u)
        return -1;
    memset((void *)(uintptr_t)d, 0, PAGE_SIZE);
    memset((void *)(uintptr_t)a, 0, PAGE_SIZE);
    memset((void *)(uintptr_t)u, 0, PAGE_SIZE);

    vq->desc  = (struct virtq_desc *)(uintptr_t)d;
    vq->avail = (struct virtq_avail *)(uintptr_t)a;
    vq->used  = (struct virtq_used *)(uintptr_t)u;
    vq->size  = size;
    vq->last_used = 0;
    vq->notify_off = 0;

    for (uint16_t i = 0; i < size; i++)
        vq->desc[i].next = i + 1;
    vq->free_head = 0;
    vq->num_free = size;
    return 0;
}

int virtq_add(struct virtq *vq, const struct virtq_buf *bufs, int n) {
    if (n <= 0 || vq->num_free < n)
        return -1;
    uint16_t head = vq->free_head;
    uint16_t cur = head;
    for (int i = 0; i < n; i++) {
        uint16_t nxt = vq->desc[cur].next;
        vq->desc[cur].addr = bufs[i].phys;
        vq->desc[cur].len  = bufs[i].len;
        vq->desc[cur].flags = bufs[i].write ? VIRTQ_DESC_F_WRITE : 0;
        if (i < n - 1) {
            vq->desc[cur].flags |= VIRTQ_DESC_F_NEXT;
            vq->desc[cur].next = nxt;     /* link to the next chain element */
        }
        cur = nxt;
    }
    vq->free_head = cur;
    vq->num_free -= n;
    return head;
}

void virtq_publish(struct virtq *vq, int head) {
    vq->avail->ring[vq->avail->idx % vq->size] = (uint16_t)head;
    virtio_wmb();                         /* slot visible before idx bump */
    vq->avail->idx++;
    virtio_wmb();
}

int virtq_pop_used(struct virtq *vq, uint32_t *len_out) {
    virtio_wmb();                         /* observe the device's idx update */
    if (vq->last_used == vq->used->idx)
        return -1;
    struct virtq_used_elem *e = &vq->used->ring[vq->last_used % vq->size];
    uint16_t head = (uint16_t)e->id;
    if (len_out)
        *len_out = e->len;
    vq->last_used++;
    return head;
}

void virtq_free_chain(struct virtq *vq, int head) {
    uint16_t cur = (uint16_t)head;
    for (;;) {
        int has_next = vq->desc[cur].flags & VIRTQ_DESC_F_NEXT;
        uint16_t nxt = vq->desc[cur].next;
        vq->desc[cur].next = vq->free_head;
        vq->free_head = cur;
        vq->num_free++;
        if (!has_next)
            break;
        cur = nxt;
    }
}
