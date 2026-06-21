/* kernel/drivers/net/virtio_net.h — virtio-net driver (NET-A). */
#pragma once
#include <stdint.h>

/* Bring up a virtio-net PCI device: negotiate, set up RX/TX virtqueues, read the
 * MAC, arm RX buffers, and verify the TX path with one frame. */
void virtio_net_init(uint8_t bus, uint8_t dev, uint8_t func);
