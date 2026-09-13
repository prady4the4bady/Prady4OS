/* kernel/drivers/input/virtio_input.h — virtio-input pointer (DDR-705).
 * Absolute pointer (virtio-tablet): IRQ-driven eventq -> current {x,y,buttons}
 * state. Dispatched from kmain's PCIe scan for vendor 0x1AF4 class 0x09. */
#pragma once
#include <stdint.h>

void virtio_input_init(uint8_t bus, uint8_t dev, uint8_t func);
/* Current pointer state mapped to screen pixels (via the GPU framebuffer
 * geometry). Returns 0 with *x,*y,*btn filled, or -1 if no pointer is up. */
int  virtio_input_state(int *x, int *y, uint32_t *buttons);
int  virtio_input_wheel(void);   /* DDR-725: read-and-clear wheel detents */
/* DDR-1026: read-and-clear the press edges not yet reported to ring 3, so a
 * press+release completed between two polls is still delivered as one click. */
uint32_t virtio_input_btn_latch(void);
