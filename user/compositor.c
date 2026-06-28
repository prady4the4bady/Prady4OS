/* user/compositor.c — PRADYOS in-house sovereign-desktop compositor (DDR-704).
 *
 * A single full-screen ring-3 process: maps the GPU framebuffer (SYS_FB_MAP),
 * renders the current mode's desktop (background + accent bar + label) with an
 * embedded 8x8 font, and runs a keyboard-driven loop (SYS_INPUT_POLL) that flips
 * the Sovereign/Manual mode (SYS_SET_MODE, needs CAP_SOVEREIGN) and re-renders.
 * NOT wlroots/Wayland — built directly on SYS_FB_* + SYS_INPUT_POLL.
 */
#include <stdio.h>

#define SYS_YIELD       3
#define SYS_EXIT        4
#define SYS_GET_MODE    29
#define SYS_SET_MODE    30
#define SYS_FB_INFO     43
#define SYS_FB_MAP      44
#define SYS_FB_FLUSH    45
#define SYS_INPUT_POLL  46

struct fb_info { unsigned width, height, stride, bpp; };

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* ---- 8x8 font: only the glyphs the mode labels use (MSB = leftmost pixel) --- */
static const unsigned char G_A[8] = {0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00};
static const unsigned char G_D[8] = {0xF8,0xCC,0xC6,0xC6,0xC6,0xCC,0xF8,0x00};
static const unsigned char G_E[8] = {0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xFE,0x00};
static const unsigned char G_G[8] = {0x7C,0xC6,0xC0,0xCE,0xC6,0xC6,0x7C,0x00};
static const unsigned char G_I[8] = {0xFE,0x18,0x18,0x18,0x18,0x18,0xFE,0x00};
static const unsigned char G_L[8] = {0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xFE,0x00};
static const unsigned char G_M[8] = {0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00};
static const unsigned char G_N[8] = {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00};
static const unsigned char G_O[8] = {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00};
static const unsigned char G_R[8] = {0xFC,0xC6,0xC6,0xFC,0xD8,0xCC,0xC6,0x00};
static const unsigned char G_S[8] = {0x7C,0xC6,0xC0,0x7C,0x06,0xC6,0x7C,0x00};
static const unsigned char G_U[8] = {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00};
static const unsigned char G_V[8] = {0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00};
static const unsigned char G_SP[8] = {0,0,0,0,0,0,0,0};

static const unsigned char *glyph(char c) {
    switch (c) {
    case 'A': return G_A; case 'D': return G_D; case 'E': return G_E;
    case 'G': return G_G; case 'I': return G_I; case 'L': return G_L;
    case 'M': return G_M; case 'N': return G_N; case 'O': return G_O;
    case 'R': return G_R; case 'S': return G_S; case 'U': return G_U;
    case 'V': return G_V; default: return G_SP;
    }
}

/* ---- framebuffer drawing ---------------------------------------------------- */
static unsigned char *g_fb;
static struct fb_info g_fi;

static void put_px(unsigned x, unsigned y, unsigned char b, unsigned char gg, unsigned char r) {
    if (x >= g_fi.width || y >= g_fi.height) return;
    unsigned char *p = g_fb + (unsigned long)y * g_fi.stride + (unsigned long)x * 4;
    p[0] = b; p[1] = gg; p[2] = r; p[3] = 0xFF;
}

static void fill_rect(unsigned x0, unsigned y0, unsigned w, unsigned h,
                      unsigned char b, unsigned char gg, unsigned char r) {
    for (unsigned y = y0; y < y0 + h; y++)
        for (unsigned x = x0; x < x0 + w; x++)
            put_px(x, y, b, gg, r);
}

static void draw_char(char c, unsigned x, unsigned y, unsigned scale,
                      unsigned char b, unsigned char gg, unsigned char r) {
    const unsigned char *gl = glyph(c);
    for (unsigned row = 0; row < 8; row++)
        for (unsigned col = 0; col < 8; col++)
            if (gl[row] & (0x80u >> col))
                fill_rect(x + col * scale, y + row * scale, scale, scale, b, gg, r);
}

static void draw_str(const char *s, unsigned x, unsigned y, unsigned scale,
                     unsigned char b, unsigned char gg, unsigned char r) {
    for (; *s; s++) {
        draw_char(*s, x, y, scale, b, gg, r);
        x += 8 * scale + scale;        /* 1px (scaled) inter-glyph gap */
    }
}

/* Render the desktop for `mode` (1 = sovereign, 0 = manual). */
static void render(int mode) {
    if (mode) {
        fill_rect(0, 0, g_fi.width, g_fi.height, 0x1A, 0x0A, 0x0A);          /* bg 0x0A0A1A */
        fill_rect(0, 0, g_fi.width, 6, 0xA8, 0x21, 0x6B);                    /* accent bar (purple) */
        draw_str("SOVEREIGN MODE", 24, 24, 3, 0xA8, 0x21, 0x6B);
    } else {
        fill_rect(0, 0, g_fi.width, g_fi.height, 0x2E, 0x1A, 0x1A);          /* bg 0x1A1A2E */
        fill_rect(0, 0, g_fi.width, 6, 0x88, 0x94, 0x0D);                    /* accent bar (teal) */
        draw_str("MANUAL MODE", 24, 24, 3, 0x88, 0x94, 0x0D);
    }
}

static int present(void) {
    for (int i = 0; i < 10; i++) {                /* tolerate a transient busy GPU */
        if (nsi(SYS_FB_FLUSH, 0, 0, 0) == 0) return 0;
        nsi(SYS_YIELD, 0, 0, 0);
    }
    return -1;
}

static void render_and_announce(int mode) {
    render(mode);
    present();
    long m = nsi(SYS_GET_MODE, 0, 0, 0);
    printf("PRADYOS_COMPOSITOR_MODE %s\n", m ? "SOVEREIGN" : "MANUAL");
    fflush(stdout);
}

int main(void) {
    if (nsi(SYS_FB_INFO, (long)&g_fi, 0, 0) != 0) {
        printf("PRADYOS_COMPOSITOR_NODEV\n");
        fflush(stdout);
        nsi(SYS_EXIT, 0, 0, 0);
    }
    long va = nsi(SYS_FB_MAP, 0, 0, 0);
    if (va < 0) {
        printf("PRADYOS_COMPOSITOR_NODEV map_rc=%ld\n", va);
        fflush(stdout);
        nsi(SYS_EXIT, 1, 0, 0);
    }
    g_fb = (unsigned char *)va;

    /* Render the first frame. fbtest (a one-shot FB consumer) may also be
     * presenting; the kernel's GPU single-flight guard serializes the two
     * control-queue submissions, and present() retries on a transient busy. */
    render((int)nsi(SYS_GET_MODE, 0, 0, 0));
    present();
    /* The first frame proves the raw SYS_FB map+draw+flush path (the smoke-fb
     * sentinel) as well as the compositor coming up. */
    printf("PRADYOS_FB_DRAW_OK %ux%u\n", g_fi.width, g_fi.height);
    printf("PRADYOS_COMPOSITOR_OK %ux%u\n", g_fi.width, g_fi.height);
    fflush(stdout);

    char keys[32];
    for (;;) {
        long n = nsi(SYS_INPUT_POLL, (long)keys, (long)sizeof keys, 0);
        for (long i = 0; i < n; i++) {
            char c = keys[i];
            if (c == 's')      { nsi(SYS_SET_MODE, 1, 0, 0); render_and_announce(1); }
            else if (c == 'm') { nsi(SYS_SET_MODE, 0, 0, 0); render_and_announce(0); }
            else if (c == 'q') { printf("PRADYOS_COMPOSITOR_EXIT\n"); fflush(stdout); nsi(SYS_EXIT, 0, 0, 0); }
        }
        nsi(SYS_YIELD, 0, 0, 0);
    }
    return 0;
}
