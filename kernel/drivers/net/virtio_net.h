/* kernel/drivers/net/virtio_net.h — virtio-net driver (NET-A). */
#pragma once
#include <stdint.h>

/* Bring up a virtio-net PCI device: negotiate, set up RX/TX virtqueues, read the
 * MAC, arm RX buffers, and verify the TX path with one frame. */
void virtio_net_init(uint8_t bus, uint8_t dev, uint8_t func);

/* NET-B: lwIP netif bridge. */
int  virtio_net_tx(const void *frame, uint32_t len);   /* send one Ethernet frame; 0 ok */
void virtio_net_set_rx(void (*cb)(const uint8_t *frame, uint32_t len)); /* RX delivery cb */
void virtio_net_mac(uint8_t out[6]);                   /* device MAC */
int  virtio_net_up(void);                              /* 1 once the device is ready */
