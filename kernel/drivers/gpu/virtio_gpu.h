/* kernel/drivers/gpu/virtio_gpu.h — VirtIO-GPU 2D framebuffer (Layer-7 slice 0,
 * ADR-028). Kernel-only bring-up: detect, mode-set scanout 0, present a linear
 * BGRA framebuffer. Dispatched from kmain's PCIe scan for class 0x03 devices. */
#pragma once
#include <stdint.h>

void virtio_gpu_init(uint8_t bus, uint8_t dev, uint8_t func);
/* The active framebuffer (identity-mapped phys==virt), or 0 if no GPU is up. */
uint8_t *virtio_gpu_fb(uint32_t *w, uint32_t *h, uint32_t *stride);
