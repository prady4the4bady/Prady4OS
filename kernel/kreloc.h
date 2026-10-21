/* kernel/kreloc.h — KASLR relocation table reservation (DDR-1179 Amendment 1).
 * The Makefile reads KRELOC_RESERVE from this line and tools/kreloc/kreloc.py
 * checks the linked .kreloc section is exactly this size. */
#pragma once

#define KRELOC_RESERVE 6144

#include <stdint.h>

/* Filled after the link by `llvm-objcopy --update-section .kreloc=`. */
extern const unsigned char kreloc_table[KRELOC_RESERVE];

/* DDR-1179 A2: running base minus link base, set by boot.asm. Never printed
 * (K1: the slot goes to the serial console only, in step 4). */
extern uint64_t kaslr_slide;

/* A code address as the LINK saw it: the slide is removed from addresses inside
 * the running image, so sym_at.sh resolves printed RIPs against kernel.elf.
 * Anything else (user RIPs, data pointers) is returned unchanged. */
uint64_t kaslr_unslide(uint64_t va);

/* Decode the table and check every recorded field holds an address inside the
 * running image; prints "[kaslr] table ok entries=N" or "... FAIL ...". */
int kreloc_selfcheck(void);
