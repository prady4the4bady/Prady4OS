/* kernel/kreloc.c — the KASLR relocation table and its users (DDR-1179 A1, A2).
 * The table is zero here; the build writes the encoded table into the linked
 * section (tools/kreloc/kreloc.py), and boot.asm applies it before kmain. */
#include "kreloc.h"
#include "console.h"

__attribute__((section(".kreloc"), used, aligned(8)))
const unsigned char kreloc_table[KRELOC_RESERVE];

uint64_t kaslr_slide;            /* .bss: boot.asm stores it after the clear */

extern char __kimg_lo[], __bss_end[];

uint64_t kaslr_unslide(uint64_t va) {
    uint64_t lo = (uint64_t)(uintptr_t)__kimg_lo, hi = (uint64_t)(uintptr_t)__bss_end;
    return (va >= lo && va < hi) ? va - kaslr_slide : va;
}

int kreloc_selfcheck(void) {
    const unsigned char *p = kreloc_table;
    uint64_t lo = (uint64_t)(uintptr_t)__kimg_lo, hi = (uint64_t)(uintptr_t)__bss_end;
    if (p[0] != 'K' || p[1] != 'R' || p[2] != 'L' || p[3] != '1') {
        kputs("[kaslr] table FAIL magic\r\n");
        return 0;
    }
    uint32_t count = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
    const unsigned char *q = p + 12, *end = p + KRELOC_RESERVE;
    uint64_t off = (uint64_t)-1, bad = 0, first_bad = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t v = 0;
        unsigned shift = 0;
        for (;;) {
            if (q >= end || shift > 56) {
                kputs("[kaslr] table FAIL truncated\r\n");
                return 0;
            }
            unsigned char b = *q++;
            v |= (uint64_t)(b & 0x7F) << shift;
            shift += 7;
            if (!(b & 0x80))
                break;
        }
        off += v >> 1;
        uint64_t field = lo + off, val;
        if (field + 8 > hi) {
            val = 0;                         /* past the image: certainly bad */
        } else if (v & 1) {
            val = *(const volatile uint64_t *)(uintptr_t)field;
        } else {
            val = (uint64_t)(int64_t)*(const volatile int32_t *)(uintptr_t)field;
        }
        if (val < lo || val > hi) {
            if (!bad) first_bad = off;
            bad++;
        }
    }
    if (bad) {
        kputs("[kaslr] table FAIL entries=");
        kputdec(count);
        kputs(" bad=");
        kputdec(bad);
        kputs(" first_bad_off=");
        kputhex(first_bad);
        kputs("\r\n");
        return 0;
    }
    kputs("[kaslr] table ok entries=");
    kputdec(count);
    kputs("\r\n");
    return 1;
}
