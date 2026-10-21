/* kernel/kreloc.h — KASLR relocation table reservation (DDR-1179 Amendment 1).
 * The Makefile reads KRELOC_RESERVE from this line and tools/kreloc/kreloc.py
 * checks the linked .kreloc section is exactly this size. */
#pragma once

#define KRELOC_RESERVE 1024

/* Filled after the link by `llvm-objcopy --update-section .kreloc=`. */
extern const unsigned char kreloc_table[KRELOC_RESERVE];
