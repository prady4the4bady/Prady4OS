/* kernel/crypto/aslr.h — entropy for the user address-space layout
 * (DDR-1189 L2, Amendment 1 P1-4). QEMU only, not verified on real hardware.
 *
 * aslr_random() never fails: rng_bytes() first, then RDRAND, then TSC readings
 * mixed into a per-boot state. The TSC path is weak and is not called secure;
 * the first time it is used the kernel prints "aslr: weak entropy" once.
 * Never use this for key material: rng_bytes() stays fail-closed for that. */
#pragma once
#include <stdint.h>

enum aslr_src { ASLR_SRC_NONE = 0, ASLR_SRC_RNG = 1, ASLR_SRC_RDRAND = 2, ASLR_SRC_TSC = 3 };

uint64_t aslr_random(void);
unsigned aslr_source(void);   /* the source of the most recent draw */
