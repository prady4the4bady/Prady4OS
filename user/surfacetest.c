/* user/surfacetest.c — two client windows + focus/key routing (DDR-706/708).
 *
 * Creates two overlapping 64x64 surfaces (A green, B blue), commits both, and
 * RAISEs B (top + focused). Then drains each surface's key ring: the compositor
 * forwards keystrokes to the focused window (B), which prints PRADYOS_FOCUS_KEY.
 */
#include <stdio.h>

#define SYS_EXIT            4
#define SYS_YIELD           3
#define SYS_SURFACE_CREATE  48
#define SYS_SURFACE_MAP     49
#define SYS_SURFACE_COMMIT  50
#define SYS_SURFACE_RAISE   54
#define SYS_SURFACE_GETKEY  56
#define SYS_SURFACE_CLOSE   59
#define SYS_SURFACE_RESIZE  60
#define SYS_SURFACE_SET_TITLE 61

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long make_window(unsigned char b, unsigned char g, unsigned char r, int x, int y) {
    long id = nsi(SYS_SURFACE_CREATE, 64, 64, 0);
    if (id < 0) return id;
    long va = nsi(SYS_SURFACE_MAP, id, 0, 0);
    if (va < 0) return va;
    unsigned char *s = (unsigned char *)va;
    for (unsigned i = 0; i < 64u * 64u; i++) {
        s[i * 4 + 0] = b; s[i * 4 + 1] = g; s[i * 4 + 2] = r; s[i * 4 + 3] = 0xFF;
    }
    nsi(SYS_SURFACE_COMMIT, id, x, y);
    return id;
}

int main(void) {
    long a = make_window(0x40, 0xE0, 0x40, 100, 100);   /* green, bottom */
    long b = make_window(0xE0, 0x40, 0x40, 140, 140);   /* blue,  raised */
    if (a < 0 || b < 0) {
        printf("PRADYOS_SURFACE_CLIENT_FAIL a=%ld b=%ld\n", a, b);
        fflush(stdout);
        nsi(SYS_EXIT, 1, 0, 0);
    }
    nsi(SYS_SURFACE_RAISE, b, 0, 0);                     /* B on top + focused */
    printf("PRADYOS_SURFACE_CLIENT_OK a=%ld b=%ld\n", a, b);
    fflush(stdout);

    /* DDR-715: name the windows (glyph-covered alphabet); the compositor draws
     * the title in the title bar. */
    if (nsi(SYS_SURFACE_SET_TITLE, a, (long)"ALPHA", 0) == 0 &&
        nsi(SYS_SURFACE_SET_TITLE, b, (long)"BETA", 0) == 0) {
        printf("PRADYOS_TITLE_OK\n");
        fflush(stdout);
    }

    /* DDR-711: a third window C exercises resize + close. C is created and resized
     * (64x64 -> 96x96) up front, but deliberately NOT raised — B keeps focus, so
     * smoke-focus/smoke-drag are unaffected. C is placed off to the side so it
     * never sits under B's title bar. It persists so the compositor composites it
     * (the live set grows to 3); after a spell we close it and the set shrinks back
     * to 2, which the compositor reports (PRADYOS_SURFACE_GONE). Meanwhile keep
     * draining the focused window's key ring (DDR-708). */
    long c = make_window(0x40, 0x40, 0xE0, 420, 70);        /* red, off to the side */
    if (c >= 0)
        nsi(SYS_SURFACE_SET_TITLE, c, (long)"GAMMA", 0);    /* DDR-715 */
    if (c >= 0 && nsi(SYS_SURFACE_RESIZE, c, 96, 96) == 0) {
        long cva = nsi(SYS_SURFACE_MAP, c, 0, 0);           /* re-map the new 96x96 buffer */
        if (cva > 0) {
            unsigned char *s = (unsigned char *)cva;
            for (unsigned i = 0; i < 96u * 96u; i++) {
                s[i*4+0] = 0x40; s[i*4+1] = 0x40; s[i*4+2] = 0xE0; s[i*4+3] = 0xFF;
            }
        }
        printf("PRADYOS_RESIZE_OK id=%ld\n", c);
        fflush(stdout);
    }

    unsigned ticks = 0;
    int closed = 0;
    for (;;) {
        long ka = nsi(SYS_SURFACE_GETKEY, a, 0, 0);
        if (ka >= 0) { printf("PRADYOS_FOCUS_KEY id=%ld ch=%c\n", a, (char)ka); fflush(stdout); }
        long kb = nsi(SYS_SURFACE_GETKEY, b, 0, 0);
        if (kb >= 0) { printf("PRADYOS_FOCUS_KEY id=%ld ch=%c\n", b, (char)kb); fflush(stdout); }

        ticks++;
        if (!closed && c >= 0 && ticks > 2000) {            /* close C; set shrinks 3 -> 2 */
            nsi(SYS_SURFACE_CLOSE, c, 0, 0);
            printf("PRADYOS_CLOSE_OK id=%ld\n", c);
            fflush(stdout);
            closed = 1;
        }
        nsi(SYS_YIELD, 0, 0, 0);
    }
    return 0;
}
