/* kernel/kreloc.c — the KASLR relocation table and its users (DDR-1179 A1, A2).
 * The table is zero here; the build writes the encoded table into the linked
 * section (tools/kreloc/kreloc.py), and boot.asm applies it before kmain. */
#include "kreloc.h"
#include "console.h"
#include "fault_expect.h"

__attribute__((section(".kreloc"), used, aligned(8)))
const unsigned char kreloc_table[KRELOC_RESERVE];

uint64_t kaslr_slide;            /* .bss: boot.asm stores it after the clear */
uint64_t kaslr_src;              /* DDR-1179 A3: 1 tsc, 2 rdrand, 3 rdseed */

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

/* ---- DDR-1179 Amendment 3: smoke-kaslr arms A, B, D ---------------------- */

__attribute__((noinline, used)) void kaslr_probe_target(void) {
    __asm__ volatile("" ::: "memory");
}

/* An R_X86_64_64 field in .rodata: the applier must have slid it. */
static void (*const g_kaslr_fptr)(void) = kaslr_probe_target;

/* Arm D: one deliberate #PF inside this function, under the fault-expect
 * latch. The unslid RIP must resolve (sym_at.sh) to this symbol. */
__attribute__((noinline)) void probe_kaslr_fault(void) {
    uint64_t lo, hi;
    __asm__ volatile("lea 1f(%%rip), %0\n\tlea 2f(%%rip), %1" : "=r"(lo), "=r"(hi));
    if (!fault_expect_arm(lo, hi, hi)) {
        kputs("[kaslr] D arm FAIL\r\n");
        return;
    }
    __asm__ volatile("1: movq 0xFFFFFF0000000000, %%rax\n2:" ::: "rax", "memory");
    uint32_t vec = 0, err = 0;
    int hit = fault_expect_taken(&vec, &err);
    kputs("[kaslr] D fault_hit=");
    kputdec((uint64_t)hit);
    kputs(" fault_rip=");
    kputhex(kaslr_unslide(fault_expect_rip()));
    kputs("\r\n");
}

static const char *src_name(uint64_t s) {
    return s == 3 ? "rdseed" : s == 2 ? "rdrand" : s == 1 ? "tsc" : "none";
}

void kaslr_report(void) {
    /* A: the .rodata pointer (R_64) and a `mov $sym` (R_32S) both equal the
     * RIP-relative address of the same function. */
    uint64_t rel, abs32;
    __asm__ volatile("lea kaslr_probe_target(%%rip), %0" : "=r"(rel));
    __asm__ volatile("movq $kaslr_probe_target, %0" : "=r"(abs32));
    uint64_t f64 = (uint64_t)(uintptr_t)g_kaslr_fptr;
    kputs("PRADYOS_KASLR_A reloc_ok=");
    kputdec(rel == f64 && rel == abs32);
    kputs("\r\n");

    /* B: applied and from which source. The slot is not in this line (K1). */
    kputs("PRADYOS_KASLR_B applied=");
    kputdec(kaslr_slide != 0);
    kputs(" src=");
    kputs(src_name(kaslr_src));
    kputs("\r\n");

    /* C's datum, serial console only (K1). */
    char b[40] = "[kaslr] slot=";
    unsigned n = 13;
    uint64_t s = kaslr_slide >> 21;
    char d[8]; int nd = 0;
    do { d[nd++] = (char)('0' + s % 10); s /= 10; } while (s);
    while (nd) b[n++] = d[--nd];
    b[n++] = '\r'; b[n++] = '\n'; b[n] = 0;
    kputs_serial_only(b);

    probe_kaslr_fault();
}
