/* kernel/mm/kreloc.h — KASLR relocation table reservation (DDR-1179 Amendment 1).
 * The Makefile reads KRELOC_RESERVE from this line and tools/kreloc/kreloc.py
 * checks the linked .kreloc section is exactly this size. 8,192 B since
 * DDR-1179 Amendment 4 (4,994 B measured for 3,864 fields; A1 had 6,144). */
#pragma once

#define KRELOC_RESERVE 8192

/* Filled after the link by `llvm-objcopy --update-section .kreloc=`. */
extern const unsigned char kreloc_table[KRELOC_RESERVE];
