/* kernel/kimg.h -- the pristine kernel image the loader left behind
 * (DDR-1143 §10.4). The installer writes these bytes; nothing else may. */
#pragma once
#include <stdint.h>

/* Copy the loader's 0x4FC0 block. Call once, early, before anything could
 * reuse that low page. Prints one [kimg] line naming the verdict. */
void kimg_capture(void);

/* On success returns 0 and the physical base + exact image length (the
 * linker's __data_end, cross-checked against the loader's size). Returns -1
 * when no valid copy exists -- the installer must refuse, never guess. */
int  kimg_get(uint64_t *phys, uint64_t *len);

/* Probe arm (QEMU_PROBES=kimg): SHA-256 over the copy, first 16 hex digits,
 * compared by the gate against the host's own sha256sum of kernel.bin. */
void kimg_probe(void);
