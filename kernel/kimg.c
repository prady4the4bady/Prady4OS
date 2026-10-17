/* kernel/kimg.c -- see kimg.h and DDR-1143 §10.4. */
#include "kimg.h"
#include "boot_info.h"
#include "console.h"
#include "crypto/sha256.h"

extern char __data_end[];
#define KERNEL_VBASE 0xFFFFFFFF80000000ull
#define KIMG_FLOOR   0x1000000ull          /* == pmm.c PMM_MIN_PHYS */

static struct boot_kimg g_kimg;
static int g_kimg_ok;

static uint64_t image_len(void) {
    return (uint64_t)(uintptr_t)__data_end - KERNEL_VBASE;
}

/* BIOS: exactly KIMG_PHYS (stage2's fixed choice). UEFI: firmware-chosen, so
 * the RANGE is checked -- page-aligned, above 1 MiB, wholly below the 16 MiB
 * PMM floor (the PMM would otherwise hand it out; the loader reports its pages
 * usable), and clear of the page tables + kernel window 0x300000..0x600000. */
static int kimg_base_ok(uint32_t src, uint64_t base, uint64_t size) {
    if (src == KIMG_SRC_BIOS)
        return base == KIMG_PHYS;
    if (base & 0xFFFu)
        return 0;
    if (base < 0x100000ull || size > KIMG_FLOOR || base > KIMG_FLOOR - size)
        return 0;
    return base + size <= 0x300000ull || base >= 0x600000ull;
}

void kimg_capture(void) {
    const struct boot_kimg *src = (const struct boot_kimg *)(uintptr_t)BOOT_KIMG_PHYS;
    g_kimg = *src;
    g_kimg_ok = 0;
    const char *why = 0;
    uint64_t len = image_len();
    if (g_kimg.magic != BOOT_KIMG_MAGIC) {
        why = "none";
    } else {
        uint32_t chk = g_kimg.magic ^ g_kimg.source ^ (uint32_t)g_kimg.base ^
                       (uint32_t)(g_kimg.base >> 32) ^ (uint32_t)g_kimg.size ^
                       (uint32_t)(g_kimg.size >> 32) ^ g_kimg.reserved;
        if (chk != g_kimg.check)
            why = "check";
        else if (!kimg_base_ok(g_kimg.source, g_kimg.base, g_kimg.size))
            why = "base";
        else if (g_kimg.source == KIMG_SRC_BIOS && g_kimg.size < len)
            why = "short";                   /* window must cover the image */
        else if (g_kimg.source == KIMG_SRC_UEFI && g_kimg.size != len)
            why = "size";                    /* the file IS the image */
        else if (g_kimg.source != KIMG_SRC_BIOS && g_kimg.source != KIMG_SRC_UEFI)
            why = "source";
    }
    kputs("[kimg] ");
    if (why) {
        kputs("refused reason=");
        kputs(why);
        kputs("\r\n");
        return;
    }
    g_kimg_ok = 1;
    kputs(g_kimg.source == KIMG_SRC_BIOS ? "src=bios" : "src=uefi");
    kputs(" len=");
    kputdec(len);
    kputs("\r\n");
}

int kimg_get(uint64_t *phys, uint64_t *len) {
    if (!g_kimg_ok)
        return -1;
    *phys = g_kimg.base;
    *len  = image_len();
    return 0;
}

void kimg_probe(void) {
    uint64_t phys, len;
    if (kimg_get(&phys, &len) != 0) {
        kputs("[kimg] probe KIMG FAIL (no copy)\r\n");
        return;
    }
    static sha256_ctx c;
    uint8_t d[SHA256_DIGEST_LEN];
    sha256_init(&c);
    sha256_update(&c, (const void *)(uintptr_t)phys, len);
    sha256_final(&c, d);
    static const char hx[] = "0123456789abcdef";
    char out[17];
    for (int i = 0; i < 8; i++) {
        out[2 * i]     = hx[d[i] >> 4];
        out[2 * i + 1] = hx[d[i] & 15];
    }
    out[16] = 0;
    kputs("[kimg] ");
    kputs(g_kimg.source == KIMG_SRC_BIOS ? "src=bios" : "src=uefi");
    kputs(" len=");
    kputdec(len);
    kputs(" sha=");
    kputs(out);
    kputs("\r\n");
}
