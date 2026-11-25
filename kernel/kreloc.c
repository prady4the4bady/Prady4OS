/* kernel/kreloc.c — the .kreloc reservation (DDR-1179 Amendment 1).
 * Zero here; the build writes the encoded relocation table into the linked
 * section (tools/kreloc/kreloc.py). Read by the boot applier (DDR-1179 sec.5,
 * step 3); nothing reads it yet. */
#include "kreloc.h"

__attribute__((section(".kreloc"), used, aligned(8)))
const unsigned char kreloc_table[KRELOC_RESERVE];

/* MUTANT K2-2: a GOTPCREL reference (R_X86_64_REX_GOTPCRELX) */
extern const unsigned char __data_end[];
const void *kreloc_mutant_got(void);
const void *kreloc_mutant_got(void) {
    const void *p;
    __asm__ volatile("movq __data_end@GOTPCREL(%%rip), %0" : "=r"(p));
    return p;
}
