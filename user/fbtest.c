/* user/fbtest.c — ring-3 framebuffer draw test (Layer 7, DDR-702).
 *
 * Proves the userspace draw->present path: query geometry (SYS_FB_INFO), map the
 * front buffer (SYS_FB_MAP), draw a deterministic pattern directly into it, and
 * present it (SYS_FB_FLUSH). With no GPU, SYS_FB_INFO returns -ENODEV and we exit
 * cleanly. Prints PRADYOS_FB_DRAW_OK <w>x<h> on success.
 */
#include <stdio.h>

#define SYS_EXIT     4
#define SYS_FB_INFO  43
#define SYS_FB_MAP   44
#define SYS_FB_FLUSH 45

struct fb_info { unsigned width, height, stride, bpp; };

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

int main(void) {
    struct fb_info fi;
    if (nsi(SYS_FB_INFO, (long)&fi, 0, 0) != 0) {
        printf("PRADYOS_FB_NODEV (no GPU framebuffer)\n");
        fflush(stdout);
        nsi(SYS_EXIT, 0, 0, 0);
    }

    long va = nsi(SYS_FB_MAP, 0, 0, 0);
    if (va < 0) {
        printf("PRADYOS_FB_MAP_FAIL rc=%ld\n", va);
        fflush(stdout);
        nsi(SYS_EXIT, 1, 0, 0);
    }

    /* Draw directly into the mapped surface: a deterministic BGRA pattern. */
    volatile unsigned char *fb = (volatile unsigned char *)va;
    for (unsigned y = 0; y < fi.height; y++) {
        volatile unsigned char *row = fb + (unsigned long)y * fi.stride;
        for (unsigned x = 0; x < fi.width; x++) {
            row[x * 4 + 0] = (unsigned char)(x ^ y);   /* B */
            row[x * 4 + 1] = (unsigned char)(y);       /* G */
            row[x * 4 + 2] = (unsigned char)(x);       /* R */
            row[x * 4 + 3] = 0xFF;                     /* A */
        }
    }

    long fr = nsi(SYS_FB_FLUSH, 0, 0, 0);
    if (fr == 0)
        printf("PRADYOS_FB_DRAW_OK %ux%u\n", fi.width, fi.height);
    else
        printf("PRADYOS_FB_FLUSH_FAIL rc=%ld\n", fr);
    fflush(stdout);
    nsi(SYS_EXIT, (fr == 0) ? 0 : 1, 0, 0);
    return 0;
}
