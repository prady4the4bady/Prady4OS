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
#define SYS_MOUSE_POLL  47
#define SYS_SURFACE_POLL 51
#define SYS_SURFACE_CMAP 52
#define SYS_AGENT_ROSTER 53
#define SYS_SURFACE_SENDKEY 55
#define SYS_CLOCK        57

struct fb_info { unsigned width, height, stride, bpp; };
struct mouse_state { int x, y; unsigned buttons; };
struct surface_info { unsigned id, w, h; int x, y, z; unsigned focused; };

/* The 8 named agents (DDR-707), in roster-slot order. */
static const char *g_agents[8] =
    { "KRYOS", "PRAX", "LUMYN", "AHNIS", "IRIS", "RUFLO", "HERMES", "SOLIN" };

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
/* Extra glyphs for the agent names (K Y P X H F). */
static const unsigned char G_K[8] = {0xC6,0xCC,0xD8,0xF0,0xD8,0xCC,0xC6,0x00};
static const unsigned char G_Y[8] = {0xC6,0xC6,0x6C,0x38,0x30,0x30,0x30,0x00};
static const unsigned char G_P[8] = {0xFC,0xC6,0xC6,0xFC,0xC0,0xC0,0xC0,0x00};
static const unsigned char G_X[8] = {0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00};
static const unsigned char G_H[8] = {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00};
static const unsigned char G_F[8] = {0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xC0,0x00};

static const unsigned char *glyph(char c) {
    switch (c) {
    case 'A': return G_A; case 'D': return G_D; case 'E': return G_E;
    case 'G': return G_G; case 'I': return G_I; case 'L': return G_L;
    case 'M': return G_M; case 'N': return G_N; case 'O': return G_O;
    case 'K': return G_K; case 'Y': return G_Y; case 'P': return G_P;
    case 'X': return G_X; case 'H': return G_H; case 'F': return G_F;
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

/* ---- OKLab ambiance engine (DDR-709) -------------------------------------- */
/* The four sun-driven ambiances (brief §1): representative bg + accent (R,G,B). */
struct ambiance { const char *name; unsigned char bg[3], ac[3]; };
static const struct ambiance AMB[4] = {
    { "DAWN",  {0x1A,0x0A,0x2E}, {0xC8,0xA4,0xE8} },
    { "DAY",   {0x0D,0x1B,0x2A}, {0x4F,0xAE,0xFF} },
    { "DUSK",  {0x3D,0x15,0x00}, {0xFF,0xB3,0x47} },
    { "NIGHT", {0x00,0x00,0x08}, {0x7B,0x4F,0xE0} },
};
static unsigned char g_bg[3] = {0x00,0x00,0x08};   /* current interpolated bg (R,G,B) */
static unsigned char g_ac[3] = {0x7B,0x4F,0xE0};   /* current interpolated accent     */

typedef struct { float L, a, b; } oklab;

static float fcbrtf(float x) {                      /* libm-free cube root (Newton) */
    if (x <= 0.0f) return 0.0f;
    float y = x;
    for (int i = 0; i < 24; i++) y = (2.0f * y + x / (y * y)) / 3.0f;
    return y;
}
static unsigned char clamp8(float v) {
    if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
    return (unsigned char)(v * 255.0f + 0.5f);
}
static oklab rgb2lab(const unsigned char c[3]) {
    float R = c[0] / 255.0f, G = c[1] / 255.0f, B = c[2] / 255.0f;
    float l = 0.4122214708f*R + 0.5363325363f*G + 0.0514459929f*B;
    float m = 0.2119034982f*R + 0.6806995451f*G + 0.1073969566f*B;
    float s = 0.0883024619f*R + 0.2817188376f*G + 0.6299787005f*B;
    float l_ = fcbrtf(l), m_ = fcbrtf(m), s_ = fcbrtf(s);
    oklab o;
    o.L = 0.2104542553f*l_ + 0.7936177850f*m_ - 0.0040720468f*s_;
    o.a = 1.9779984951f*l_ - 2.4285922050f*m_ + 0.4505937099f*s_;
    o.b = 0.0259040371f*l_ + 0.7827717662f*m_ - 0.8086757660f*s_;
    return o;
}
static void lab2rgb(oklab o, unsigned char out[3]) {
    float l_ = o.L + 0.3963377774f*o.a + 0.2158037573f*o.b;
    float m_ = o.L - 0.1055613458f*o.a - 0.0638541728f*o.b;
    float s_ = o.L - 0.0894841775f*o.a - 1.2914855480f*o.b;
    float l = l_*l_*l_, m = m_*m_*m_, s = s_*s_*s_;
    out[0] = clamp8( 4.0767416621f*l - 3.3077115913f*m + 0.2309699292f*s);
    out[1] = clamp8(-1.2684380046f*l + 2.6097574011f*m - 0.3413193965f*s);
    out[2] = clamp8(-0.0041960863f*l - 0.7034186147f*m + 1.7076147010f*s);
}
/* Interpolate a..b in OKLab at t in [0,1] -> sRGB out. */
static void lab_lerp(const unsigned char a[3], const unsigned char b[3], float t, unsigned char out[3]) {
    oklab A = rgb2lab(a), B = rgb2lab(b), m;
    m.L = A.L + (B.L - A.L) * t;
    m.a = A.a + (B.a - A.a) * t;
    m.b = A.b + (B.b - A.b) * t;
    lab2rgb(m, out);
}
static int ambiance_for_secs(long secs) {           /* §1 boundaries by hour */
    int h = (int)(secs / 3600) % 24;
    if (h >= 5 && h <= 8)  return 0;                /* DAWN  */
    if (h >= 9 && h <= 16) return 1;                /* DAY   */
    if (h >= 17 && h <= 20) return 2;               /* DUSK  */
    return 3;                                       /* NIGHT */
}

/* Agent panel (DDR-707): the 8 named agents as cards on the right, each with a
 * status dot — green if AETHER's roster marks it active, dim otherwise. */
static void render_agent_panel(void) {
    unsigned char roster[8] = {0};
    nsi(SYS_AGENT_ROSTER, (long)roster, 8, 0);
    if (g_fi.width < 220) return;
    unsigned px = g_fi.width - 210;
    for (int i = 0; i < 8; i++) {
        unsigned py = 70 + (unsigned)i * 44;
        fill_rect(px, py, 200, 36, 0x30, 0x18, 0x20);            /* card bg */
        draw_str(g_agents[i], px + 10, py + 14, 1, 0xE0, 0xE0, 0xF0);
        if (roster[i]) fill_rect(px + 178, py + 12, 12, 12, 0x40, 0xE0, 0x40);  /* active = green */
        else           fill_rect(px + 178, py + 12, 12, 12, 0x50, 0x50, 0x50);  /* inactive = gray */
    }
}

/* Render the desktop: the current ambiance bg + accent (DDR-709), the mode label
 * (DDR-704), and the agent panel (DDR-707). Colours are BGRA; g_bg/g_ac are RGB. */
static void render(int mode) {
    fill_rect(0, 0, g_fi.width, g_fi.height, g_bg[2], g_bg[1], g_bg[0]);  /* ambiance bg */
    fill_rect(0, 0, g_fi.width, 6, g_ac[2], g_ac[1], g_ac[0]);            /* accent bar  */
    draw_str(mode ? "SOVEREIGN MODE" : "MANUAL MODE", 24, 24, 3,
             g_ac[2], g_ac[1], g_ac[0]);
    render_agent_panel();                                                /* DDR-707 */
}

/* Blit a client surface (mapped at sva, w x h BGRA) onto the FB at (dx,dy). */
static void blit_surface(const unsigned char *sva, unsigned w, unsigned h, int dx, int dy) {
    for (unsigned yy = 0; yy < h; yy++) {
        for (unsigned xx = 0; xx < w; xx++) {
            int px = dx + (int)xx, py = dy + (int)yy;
            if (px < 0 || py < 0 || (unsigned)px >= g_fi.width || (unsigned)py >= g_fi.height)
                continue;
            const unsigned char *s = sva + ((unsigned long)yy * w + xx) * 4;
            put_px((unsigned)px, (unsigned)py, s[0], s[1], s[2]);
        }
    }
}

/* A small white cursor block at (x,y), clamped to the screen. */
static void draw_cursor(int x, int y) {
    if (x < 0) x = 0; if (y < 0) y = 0;
    if ((unsigned)x > g_fi.width - 12) x = (int)g_fi.width - 12;
    if ((unsigned)y > g_fi.height - 12) y = (int)g_fi.height - 12;
    fill_rect((unsigned)x, (unsigned)y, 12, 12, 0xFF, 0xFF, 0xFF);
}

static int present(void) {
    for (int i = 0; i < 10; i++) {                /* tolerate a transient busy GPU */
        if (nsi(SYS_FB_FLUSH, 0, 0, 0) == 0) return 0;
        nsi(SYS_YIELD, 0, 0, 0);
    }
    return -1;
}

static int g_cur_amb = 3;                            /* current ambiance index (NIGHT) */

/* Transition the ambiance bg+accent to AMB[idx] over `frames`, OKLab-interpolated. */
static void set_ambiance(int idx, int frames) {
    if (idx < 0 || idx > 3) return;
    unsigned char fbg[3], fac[3];
    for (int i = 0; i < 3; i++) { fbg[i] = g_bg[i]; fac[i] = g_ac[i]; }
    for (int f = 1; f <= frames; f++) {
        float t = (float)f / (float)frames;
        lab_lerp(fbg, AMB[idx].bg, t, g_bg);
        lab_lerp(fac, AMB[idx].ac, t, g_ac);
        render((int)nsi(SYS_GET_MODE, 0, 0, 0));
        present();
    }
    g_cur_amb = idx;
}

/* Animated toggle (DDR-709): pulse the accent toward white and back in OKLab. */
static void animate_toggle(void) {
    static const unsigned char white[3] = {0xFF, 0xFF, 0xFF};
    unsigned char base[3];
    for (int i = 0; i < 3; i++) base[i] = g_ac[i];
    int mode = (int)nsi(SYS_GET_MODE, 0, 0, 0);
    for (int f = 1; f <= 6; f++) { lab_lerp(base, white, (float)f / 6.0f, g_ac); render(mode); present(); }
    for (int f = 6; f >= 0; f--) { lab_lerp(base, white, (float)f / 6.0f, g_ac); render(mode); present(); }
    printf("PRADYOS_TOGGLE_ANIM_OK\n");
    fflush(stdout);
}

static void render_and_announce(int mode) {
    animate_toggle();                               /* DDR-709: animated toggle */
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

    /* DDR-709: one-time demo cycle through the 4 ambiances (OKLab transitions),
     * then settle on the time-of-day ambiance from the RTC. */
    for (int k = 0; k < 4; k++) {
        set_ambiance(k, 6);
        printf("PRADYOS_AMBIANCE %s\n", AMB[k].name);
        fflush(stdout);
    }
    printf("PRADYOS_AMBIANCE_OK\n");
    fflush(stdout);
    set_ambiance(ambiance_for_secs(nsi(SYS_CLOCK, 0, 0, 0)), 6);

    char keys[32];
    unsigned prev_btn = 0;
    long composited = 0;             /* count of client surfaces last composited */
    int focus_id = -1, last_focus = -2;       /* focused surface (DDR-708) */
    unsigned char last_roster[8] = {0xFF};   /* force a first-read print */
    for (;;) {
        /* DDR-709: real-time sun-driven ambiance — transition at hour boundaries. */
        int amb = ambiance_for_secs(nsi(SYS_CLOCK, 0, 0, 0));
        if (amb != g_cur_amb) set_ambiance(amb, 8);
        /* Named-agent panel (DDR-707): when AETHER's roster changes, re-render
         * (the panel is part of render()) and report the roster to serial. */
        unsigned char roster[8] = {0};
        nsi(SYS_AGENT_ROSTER, (long)roster, 8, 0);
        int changed = 0;
        for (int i = 0; i < 8; i++) if (roster[i] != last_roster[i]) changed = 1;
        if (changed) {
            render((int)nsi(SYS_GET_MODE, 0, 0, 0));
            present();
            for (int i = 0; i < 8; i++)
                printf("AGENT %s %s\n", g_agents[i], roster[i] ? "active" : "inactive");
            printf("PRADYOS_AGENTS_OK\n");
            fflush(stdout);
            for (int i = 0; i < 8; i++) last_roster[i] = roster[i];
        }
        /* Per-client surfaces (DDR-706/708): SURFACE_POLL is z-sorted (back-to-front).
         * Re-composite when the set grows or focus changes; blit in z-order so a
         * raised window is on top, and report the z-order + focused window. */
        struct surface_info surfs[16];
        long ns = nsi(SYS_SURFACE_POLL, (long)surfs, 16, 0);
        int cur_focus = -1;
        for (long i = 0; i < ns; i++) if (surfs[i].focused) cur_focus = (int)surfs[i].id;
        focus_id = cur_focus;
        if (ns > composited || cur_focus != last_focus) {
            render((int)nsi(SYS_GET_MODE, 0, 0, 0));
            for (long i = 0; i < ns; i++) {                 /* z-order: bottom..top */
                long sva = nsi(SYS_SURFACE_CMAP, (long)surfs[i].id, 0, 0);
                if (sva > 0)
                    blit_surface((const unsigned char *)sva, surfs[i].w, surfs[i].h,
                                 surfs[i].x, surfs[i].y);
            }
            present();
            if (ns > 0) {
                printf("PRADYOS_ZORDER");
                for (long i = 0; i < ns; i++) printf(" %u", surfs[i].id);
                printf("\n");
            }
            for (long i = composited; i < ns; i++)
                printf("PRADYOS_SURFACE_OK %u\n", surfs[i].id);
            if (cur_focus >= 0 && cur_focus != last_focus)
                printf("PRADYOS_FOCUS id=%d\n", cur_focus);
            fflush(stdout);
            composited = ns;
            last_focus = cur_focus;
        }
        long n = nsi(SYS_INPUT_POLL, (long)keys, (long)sizeof keys, 0);
        for (long i = 0; i < n; i++) {
            char c = keys[i];
            if (c == 's')      { nsi(SYS_SET_MODE, 1, 0, 0); render_and_announce(1); }
            else if (c == 'm') { nsi(SYS_SET_MODE, 0, 0, 0); render_and_announce(0); }
            else if (c == 'q') { printf("PRADYOS_COMPOSITOR_EXIT\n"); fflush(stdout); nsi(SYS_EXIT, 0, 0, 0); }
            else if (focus_id >= 0)                          /* DDR-708: route to focus */
                nsi(SYS_SURFACE_SENDKEY, focus_id, (long)c, 0);
        }
        /* Pointer (DDR-705): on a button-down, redraw the desktop with a cursor at
         * the pointer position and present it (event-driven — no continuous flush). */
        struct mouse_state ms;
        if (nsi(SYS_MOUSE_POLL, (long)&ms, 0, 0) == 0) {
            if (ms.buttons && !prev_btn) {
                render((int)nsi(SYS_GET_MODE, 0, 0, 0));
                draw_cursor(ms.x, ms.y);
                present();
                printf("PRADYOS_MOUSE_OK %d %d\n", ms.x, ms.y);
                fflush(stdout);
            }
            prev_btn = ms.buttons;
        }
        nsi(SYS_YIELD, 0, 0, 0);
    }
    return 0;
}
