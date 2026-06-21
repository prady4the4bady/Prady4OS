/* kernel/drivers/net/netbuf.h — fixed packet-buffer pool for virtio-net (NET-A).
 *
 * A small LIFO pool of page-sized, identity-mapped (phys == virt) DMA buffers,
 * allocated once at init so the RX/TX paths never call the allocator from an
 * interrupt handler. Each buffer holds a virtio_net_hdr + an Ethernet frame.
 */
#pragma once
#include <stdint.h>

#define NETBUF_SIZE 2048u            /* usable bytes per buffer */

void     netbuf_init(void);          /* allocate the pool (idempotent) */
uint64_t netbuf_alloc(void);         /* a buffer's phys/virt address, or 0 */
void     netbuf_free(uint64_t buf);  /* return a buffer to the pool */
