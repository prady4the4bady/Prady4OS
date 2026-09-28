/* kernel/drivers/gpu/display.h -- one display source, two backends (DDR-1142).
 *
 * virtio-gpu when it is up, otherwise the UEFI GOP framebuffer the loader
 * handed over. Every consumer of "the framebuffer" (sys_fb, the pointer's
 * absolute-coordinate scaling) asks here, never a backend directly, so a machine
 * with no virtio-gpu still has a desktop if its firmware set a video mode.
 */
#pragma once
#include <stdint.h>

/* Read the loader's boot_fb block. Call once, early, while the low page is
 * still exactly as the loader left it. Records only; maps nothing. */
void display_capture_boot(void);

/* After PCIe enumeration (so virtio-gpu has had its chance): choose the
 * backend, map the GOP framebuffer if it is the one, print one "[fb] gop" line,
 * and -- under the "gop" probe -- draw the four-quadrant test pattern. */
void display_init(void);

/* The active framebuffer: kernel pointer, geometry, stride in BYTES, and the
 * PHYSICAL address of the first pixel (not page-aligned in general). Returns 0
 * when there is no display. Any out-pointer may be 0. */
uint8_t *display_fb(uint32_t *w, uint32_t *h, uint32_t *stride_bytes,
                    uint64_t *phys);

/* Push the framebuffer to the scanout. 0 on success. A GOP framebuffer IS the
 * scanout, so there is nothing to push and it always succeeds. */
int display_present(void);
