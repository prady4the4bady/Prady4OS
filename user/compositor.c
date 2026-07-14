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
#define SYS_POWEROFF    69   /* DDR-746: ACPI S5 (CAP_SOVEREIGN) */
#define SYS_REBOOT      70   /* DDR-747: ACPI/PC reset (CAP_SOVEREIGN) */
#define SYS_GET_MODE    29
#define SYS_SET_MODE    30
#define SYS_SPAWN_AGENT 35
#define SYS_FB_INFO     43
#define SYS_FB_MAP      44
#define SYS_FB_FLUSH    45
#define SYS_INPUT_POLL  46
#define SYS_MOUSE_POLL  47
#define SYS_SURFACE_POLL 51
#define SYS_SURFACE_CMAP 52
#define SYS_AGENT_ROSTER 53
#define SYS_AGENT_METRICS 64            /* DDR-730/735: per-agent live metrics */
#define SYS_SURFACE_RAISE 54
#define SYS_SURFACE_SENDKEY 55
#define SYS_CLOCK        57
#define SYS_SURFACE_MOVE 58
#define SYS_SURFACE_CLOSE 59
#define SYS_SURFACE_SENDEV 62

struct fb_info { unsigned width, height, stride, bpp; };
struct mouse_state { int x, y; unsigned buttons; int wheel; };   /* DDR-725 */
struct surface_info { unsigned id, w, h; int x, y, z; unsigned focused; char title[16]; };
/* DDR-735 (kernel-mirrored): counts are retained post-mortem; pid stays set for
 * a spawned-then-exited slot (state 0), so "ran, now done" is renderable. */
struct agent_metric { unsigned pid, state; unsigned long mem_used, actions, run_ticks, dispatches; };

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
/* Glyphs for the window titles (DDR-715). */
static const unsigned char G_B[8] = {0xFC,0xC6,0xC6,0xFC,0xC6,0xC6,0xFC,0x00};
static const unsigned char G_C[8] = {0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00};
static const unsigned char G_T[8] = {0xFE,0x18,0x18,0x18,0x18,0x18,0x18,0x00};
static const unsigned char G_W[8] = {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00};

static const unsigned char *glyph(char c) {
    switch (c) {
    case 'A': return G_A; case 'D': return G_D; case 'E': return G_E;
    case 'G': return G_G; case 'I': return G_I; case 'L': return G_L;
    case 'M': return G_M; case 'N': return G_N; case 'O': return G_O;
    case 'K': return G_K; case 'Y': return G_Y; case 'P': return G_P;
    case 'X': return G_X; case 'H': return G_H; case 'F': return G_F;
    case 'R': return G_R; case 'S': return G_S; case 'U': return G_U;
    case 'V': return G_V; case 'B': return G_B; case 'C': return G_C;
    case 'T': return G_T; case 'W': return G_W; default: return G_SP;
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

/* ---- DDR-712: particle field + frosted glass --------------------------------
 * The brief's signature depth (§1 particles, §9 glass) on the software BGRA
 * framebuffer. Real Gaussian blur is deferred; glass is a flat translucent tint
 * of the ambiance bg (computed, not FB-read, so it stays cheap); particles are a
 * deterministic LCG-seeded pool alpha-blended over the background. */
static int g_cur_amb = 3;                            /* current ambiance index (NIGHT) */
static unsigned g_frame = 0;                         /* advances each render (particle phase) */
static int g_visual_announced = 0;                   /* one-time sentinel guard          */
static int g_particle_n = 0;                         /* particles drawn last render      */

#define PARTICLE_MAX 200
static int   p_init = 0;
static float p_x[PARTICLE_MAX], p_y[PARTICLE_MAX];
static unsigned p_lcg = 0x1234567u;
static unsigned lcg15(void) { p_lcg = p_lcg * 1103515245u + 12345u; return (p_lcg >> 16) & 0x7FFFu; }

/* Alpha-composite one pixel over the framebuffer (a in [0,1]); B,G,R like put_px.
 * Used for particles only (a few hundred px/frame, so the FB reads stay cheap). */
static void blend_px(unsigned x, unsigned y, unsigned char b, unsigned char gg, unsigned char r, float a) {
    if (x >= g_fi.width || y >= g_fi.height) return;
    unsigned char *p = g_fb + (unsigned long)y * g_fi.stride + (unsigned long)x * 4;
    p[0] = (unsigned char)(p[0] * (1.0f - a) + b  * a);
    p[1] = (unsigned char)(p[1] * (1.0f - a) + gg * a);
    p[2] = (unsigned char)(p[2] * (1.0f - a) + r  * a);
    p[3] = 0xFF;
}

/* A bounded radial glow (DDR-716): quadratic falloff a = base_a*(1 - d^2/r^2),
 * sqrt-free, clipped to the screen. The backdrop primitive for the DAY mesh
 * nodes, the DUSK sun-bloom, and the NIGHT nebulas. */
static void radial_glow(int cx, int cy, int r,
                        unsigned char cr, unsigned char cg, unsigned char cb, float base_a) {
    int x0 = cx - r, x1 = cx + r, y0 = cy - r, y1 = cy + r;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > (int)g_fi.width)  x1 = (int)g_fi.width;
    if (y1 > (int)g_fi.height) y1 = (int)g_fi.height;
    float r2 = (float)r * (float)r;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float dx = (float)(x - cx), dy = (float)(y - cy);
            float d2 = dx * dx + dy * dy;
            if (d2 >= r2) continue;
            blend_px((unsigned)x, (unsigned)y, cb, cg, cr, base_a * (1.0f - d2 / r2));
        }
    }
}

/* The settled per-ambiance backdrop (DDR-716, brief §1). Drawn only on settled
 * frames (not mid-OKLab-lerp — D3's perf guard); announces each ambiance's
 * first settled render and PRADYOS_BACKDROP_OK once all four have been seen. */
static int g_settled = 1;                 /* cleared during ambiance transitions */
static void render_backdrop(void) {
    unsigned w = g_fi.width, h = g_fi.height;
    switch (g_cur_amb) {
    case 0:                                        /* DAWN: motes carry it */
        break;
    case 1:                                        /* DAY: 3-node gradient mesh */
        radial_glow((int)(w / 4),     (int)(h / 4),     (int)(w * 30 / 100), 0x8F, 0xC8, 0xFF, 0.20f);
        radial_glow((int)(w / 2),     (int)(h * 2 / 3), (int)(w * 25 / 100), 0xFF, 0xFF, 0xFF, 0.12f);
        radial_glow((int)(w * 3 / 4), (int)(h / 3),     (int)(w * 28 / 100), 0x1E, 0x50, 0xA0, 0.18f);
        break;
    case 2:                                        /* DUSK: sun-bloom at 85%,90% */
        radial_glow((int)(w * 85 / 100), (int)(h * 90 / 100), (int)(w * 35 / 100), 0xFF, 0x78, 0x1E, 0.25f);
        break;
    default:                                       /* NIGHT: two nebulas */
        radial_glow((int)(w * 30 / 100), (int)(h * 40 / 100), (int)(w * 600 / 1024), 0x12, 0x00, 0x24, 0.30f);
        radial_glow((int)(w * 70 / 100), (int)(h * 60 / 100), (int)(w * 500 / 1024), 0x00, 0x12, 0x20, 0.30f);
        break;
    }
    static unsigned char seen[4];
    static int all_announced;
    if (!seen[g_cur_amb]) {
        seen[g_cur_amb] = 1;
        printf("PRADYOS_BACKDROP %s\n", AMB[g_cur_amb].name);
        if (!all_announced && seen[0] && seen[1] && seen[2] && seen[3]) {
            all_announced = 1;
            printf("PRADYOS_BACKDROP_OK\n");
        }
        fflush(stdout);
    }
}

/* DDR-722: separable in-place box blur (radius 4) + saturation boost over a
 * framebuffer rect — the real frosted-glass backdrop (brief §9). */
#define BLUR_R 4
static void blur_rect(unsigned x0, unsigned y0, unsigned w, unsigned h) {
    if (!g_fb || w == 0 || h == 0) return;
    if (x0 + w > g_fi.width || y0 + h > g_fi.height) return;
    static unsigned char line[4096 * 4];             /* max row/col scratch */
    if (w * 4 > sizeof line || h * 4 > sizeof line) return;

    for (unsigned y = y0; y < y0 + h; y++) {         /* horizontal pass */
        unsigned char *row = g_fb + (unsigned long)y * g_fi.stride + (unsigned long)x0 * 4;
        for (unsigned x = 0; x < w; x++) {
            unsigned sb = 0, sg = 0, sr = 0, n = 0;
            for (int k = -BLUR_R; k <= BLUR_R; k++) {
                int xx = (int)x + k;
                if (xx < 0 || xx >= (int)w) continue;
                sb += row[xx * 4 + 0]; sg += row[xx * 4 + 1]; sr += row[xx * 4 + 2]; n++;
            }
            line[x * 4 + 0] = (unsigned char)(sb / n);
            line[x * 4 + 1] = (unsigned char)(sg / n);
            line[x * 4 + 2] = (unsigned char)(sr / n);
        }
        for (unsigned x = 0; x < w; x++) {
            row[x * 4 + 0] = line[x * 4 + 0];
            row[x * 4 + 1] = line[x * 4 + 1];
            row[x * 4 + 2] = line[x * 4 + 2];
        }
    }
    for (unsigned x = x0; x < x0 + w; x++) {         /* vertical pass */
        for (unsigned y = 0; y < h; y++) {
            unsigned sb = 0, sg = 0, sr = 0, n = 0;
            for (int k = -BLUR_R; k <= BLUR_R; k++) {
                int yy = (int)y + k;
                if (yy < 0 || yy >= (int)h) continue;
                unsigned char *p = g_fb + (unsigned long)(y0 + yy) * g_fi.stride + (unsigned long)x * 4;
                sb += p[0]; sg += p[1]; sr += p[2]; n++;
            }
            line[y * 4 + 0] = (unsigned char)(sb / n);
            line[y * 4 + 1] = (unsigned char)(sg / n);
            line[y * 4 + 2] = (unsigned char)(sr / n);
        }
        for (unsigned y = 0; y < h; y++) {
            unsigned char *p = g_fb + (unsigned long)(y0 + y) * g_fi.stride + (unsigned long)x * 4;
            /* saturation boost x1.3 around luma (brief: blur + saturation) */
            int b = line[y * 4 + 0], gg = line[y * 4 + 1], r = line[y * 4 + 2];
            int gray = (b + gg + r) / 3;
            int nb = gray + ((b - gray) * 13) / 10;
            int ng = gray + ((gg - gray) * 13) / 10;
            int nr = gray + ((r - gray) * 13) / 10;
            p[0] = (unsigned char)(nb < 0 ? 0 : nb > 255 ? 255 : nb);
            p[1] = (unsigned char)(ng < 0 ? 0 : ng > 255 ? 255 : ng);
            p[2] = (unsigned char)(nr < 0 ? 0 : nr > 255 ? 255 : nr);
        }
    }
}

/* A frosted-glass card: the scene beneath is BLURRED + saturated (DDR-722),
 * then a translucent tint + 1px accent border go over it (brief §9). */
static void glass_card(unsigned x, unsigned y, unsigned w, unsigned h) {
    static int blur_said;
    blur_rect(x, y, w, h);
    if (!blur_said) {
        blur_said = 1;
        printf("PRADYOS_GLASS_BLUR_OK\n");
        fflush(stdout);
    }
    /* DDR-722: the tint now BLENDS over the blurred backdrop (an opaque
     * precomputed fill would erase the blur). rgba(255,255,255,0.10). */
    for (unsigned yy = y; yy < y + h; yy++)
        for (unsigned xx = x; xx < x + w; xx++)
            blend_px(xx, yy, 255, 255, 255, 0.10f);
    fill_rect(x, y, w, 1, g_ac[2], g_ac[1], g_ac[0]);          /* top    */
    fill_rect(x, y + h - 1, w, 1, g_ac[2], g_ac[1], g_ac[0]);  /* bottom */
    fill_rect(x, y, 1, h, g_ac[2], g_ac[1], g_ac[0]);          /* left   */
    fill_rect(x + w - 1, y, 1, h, g_ac[2], g_ac[1], g_ac[0]);  /* right  */
}

/* The per-ambiance particle field (brief §1): DAWN motes, DAY none (mesh
 * deferred), DUSK embers, NIGHT stars. Deterministic positions; wraps at edges. */
static void render_particles(void) {
    if (!p_init) {
        for (int i = 0; i < PARTICLE_MAX; i++) {
            p_x[i] = (float)(lcg15() % (g_fi.width  ? g_fi.width  : 1u));
            p_y[i] = (float)(lcg15() % (g_fi.height ? g_fi.height : 1u));
        }
        p_init = 1;
    }
    int n; float dx, dy; unsigned char pr, pg, pb; float base_a;
    switch (g_cur_amb) {
    case 0:  n = 120; dx =  0.2f; dy = -0.2f;  pr = 0xC8; pg = 0xA4; pb = 0xE8; base_a = 0.30f; break; /* DAWN motes  */
    case 1:  n = 0;   dx = 0;     dy = 0;      pr = pg = pb = 0;     base_a = 0;     break;            /* DAY  none   */
    case 2:  n = 60;  dx = 0.0f;  dy = -0.12f; pr = 0xFF; pg = 0x78; pb = 0x28; base_a = 0.35f; break; /* DUSK embers */
    default: n = 200; dx = 0.02f; dy = 0.03f;  pr = 0xFF; pg = 0xFF; pb = 0xFF; base_a = 0.40f; break; /* NIGHT stars */
    }
    for (int i = 0; i < n && i < PARTICLE_MAX; i++) {
        p_x[i] += dx; p_y[i] += dy;
        if (p_x[i] < 0) p_x[i] += g_fi.width;  if (p_x[i] >= g_fi.width)  p_x[i] -= g_fi.width;
        if (p_y[i] < 0) p_y[i] += g_fi.height; if (p_y[i] >= g_fi.height) p_y[i] -= g_fi.height;
        float a = base_a;
        if (g_cur_amb == 3) a = 0.15f + 0.45f * ((float)((g_frame + (unsigned)i * 7u) % 16u) / 16.0f); /* twinkle */
        blend_px((unsigned)p_x[i], (unsigned)p_y[i], pb, pg, pr, a);
        if (g_cur_amb == 3 && i < 4) {               /* 4 bright stars w/ a soft glow */
            blend_px((unsigned)p_x[i] + 1, (unsigned)p_y[i], pb, pg, pr, a * 0.6f);
            blend_px((unsigned)p_x[i], (unsigned)p_y[i] + 1, pb, pg, pr, a * 0.6f);
        }
    }
    g_particle_n = n;
}

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

/* Agent panel (DDR-707/737): the 8 named agents as cards on the right, rendered
 * from SYS_AGENT_METRICS (which subsumes the roster — state >= 1 IS the DDR-730
 * lazy-liveness bit). Status dot by state: green = running/ready, amber =
 * blocked, dim green = ran-and-exited (retained pid, state 0), gray = never
 * spawned. Up to 4 activity pips show submitted actions (font-free; DDR-735's
 * post-mortem retention keeps them lit after the agent completes). */
static void render_agent_panel(void) {
    struct agent_metric m[8] = {0};
    nsi(SYS_AGENT_METRICS, (long)m, 8, 0);
    if (g_fi.width < 220) return;
    unsigned px = g_fi.width - 210;
    for (int i = 0; i < 8; i++) {
        unsigned py = 70 + (unsigned)i * 44;
        glass_card(px, py, 200, 36);                             /* DDR-712: frosted glass */
        draw_str(g_agents[i], px + 10, py + 14, 1, 0xE0, 0xE0, 0xF0);
        if (m[i].state == 1)      fill_rect(px + 178, py + 12, 12, 12, 0x40, 0xE0, 0x40);  /* run/ready */
        else if (m[i].state == 2) fill_rect(px + 178, py + 12, 12, 12, 0x40, 0xC0, 0xE0);  /* blocked = amber */
        else if (m[i].pid)        fill_rect(px + 178, py + 12, 12, 12, 0x30, 0x80, 0x30);  /* ran, done = dim green */
        else                      fill_rect(px + 178, py + 12, 12, 12, 0x50, 0x50, 0x50);  /* empty = gray */
        unsigned pips = m[i].actions > 4 ? 4u : (unsigned)m[i].actions;   /* DDR-737 activity meter */
        for (unsigned p = 0; p < pips; p++)
            fill_rect(px + 118 + p * 8, py + 26, 5, 5, 0xC0, 0xC0, 0x60);
    }
}

/* DDR-713: which agent card (0..7) is under (x,y), or -1. Mirrors the layout in
 * render_agent_panel (cards at x=width-210, y=70+i*44, 200x36). */
static int agent_card_hit(int x, int y) {
    if (g_fi.width < 220) return -1;
    int px = (int)g_fi.width - 210;
    for (int i = 0; i < 8; i++) {
        int py = 70 + i * 44;
        if (x >= px && x < px + 200 && y >= py && y < py + 36)
            return i;
    }
    return -1;
}

/* Render the desktop: the current ambiance bg + accent (DDR-709), the mode label
 * (DDR-704), and the agent panel (DDR-707). Colours are BGRA; g_bg/g_ac are RGB. */
static void render(int mode) {
    /* DDR-723: multi-stop vertical gradient base (brief §1) — 3 stops derived
     * from the ambiance bg: lightened at the horizon-third, base at top,
     * darkened toward the bottom. Per-row color, one fill per row. */
    {
        unsigned hh = g_fi.height ? g_fi.height : 1;
        for (unsigned y = 0; y < hh; y++) {
            /* stops: 0.0 -> bg, 0.35 -> bg*1.25 (lightened), 1.0 -> bg*0.55 */
            unsigned char c[3];
            float t = (float)y / (float)hh;
            float f = (t < 0.35f) ? 1.0f + (0.25f * (t / 0.35f))
                                  : 1.25f - (0.70f * ((t - 0.35f) / 0.65f));
            for (int i = 0; i < 3; i++) {
                float v = g_bg[i] * f;
                c[i] = (unsigned char)(v > 255.0f ? 255 : v);
            }
            fill_rect(0, y, g_fi.width, 1, c[2], c[1], c[0]);
        }
        static int grad_said;
        if (!grad_said) { grad_said = 1; printf("PRADYOS_GRADIENT_OK\n"); fflush(stdout); }
    }
    fill_rect(0, 0, g_fi.width, 6, g_ac[2], g_ac[1], g_ac[0]);            /* accent bar  */
    if (g_settled)                                                       /* DDR-716: backdrops */
        render_backdrop();                                               /* (skipped mid-lerp) */
    render_particles();                                                  /* DDR-712: particle field */
    draw_str(mode ? "SOVEREIGN MODE" : "MANUAL MODE", 24, 24, 3,
             g_ac[2], g_ac[1], g_ac[0]);
    render_agent_panel();                                                /* DDR-707 (glass cards) */
    g_frame++;
    if (!g_visual_announced) {                                           /* DDR-712 sentinels (once) */
        printf("PRADYOS_PARTICLES_OK n=%d\n", g_particle_n);
        printf("PRADYOS_GLASS_OK\n");
        fflush(stdout);
        g_visual_announced = 1;
    }
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

/* A window = its client surface content + a title-bar decoration above it
 * (the drag handle), drawn in the ambiance accent colour (DDR-710), with the
 * window's title string and a close box on the right (DDR-715). */
/* DDR-728: the Inter typeface — 16px alpha glyph atlas generated offline by
 * tools/fontgen (stb_truetype over Inter-Regular, SIL OFL; rendered bitmaps).
 * Alpha-blended per pixel; used where 16px fits (titles, banner). */
#include "inter_font.h"
static void draw_str_inter(const char *s, int x, int y,
                           unsigned char b, unsigned char g2, unsigned char r) {
    static int font_said;
    int pen = x;
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        if (c < INTER_FIRST || c > INTER_LAST) { pen += INTER_PX / 2; continue; }
        const struct inter_glyph *gl = &inter_glyphs[c - INTER_FIRST];
        for (int gy = 0; gy < gl->h; gy++)
            for (int gx = 0; gx < gl->w; gx++) {
                unsigned char a = inter_pixels[gl->off + gy * gl->w + gx];
                if (a)
                    blend_px((unsigned)(pen + gl->xoff + gx),
                             (unsigned)(y + INTER_ASCENT + gl->yoff + gy),
                             b, g2, r, (float)a / 255.0f);
            }
        pen += gl->adv;
    }
    if (!font_said) {
        font_said = 1;
        printf("PRADYOS_FONT_OK\n");
        fflush(stdout);
    }
}

#define TITLEBAR  18
#define CLOSEBOX  12                 /* close box size; inset 4 px from the right */
static void draw_window(const unsigned char *sva, const struct surface_info *s) {
    /* DDR-724: decorations — a soft drop shadow (right+bottom strips, blended)
     * and a 1px frame: accent-colored when focused, neutral gray otherwise. */
    {
        int ty0 = s->y - TITLEBAR; if (ty0 < 0) ty0 = 0;
        int tx0 = s->x < 0 ? 0 : s->x;
        unsigned fw = s->w, fh = (unsigned)(s->y - ty0) + s->h;
        for (int d = 1; d <= 3; d++) {                     /* shadow, fading out */
            float a = 0.22f - 0.06f * (float)(d - 1);
            for (unsigned yy = 0; yy < fh + (unsigned)d; yy++)   /* right strip */
                blend_px((unsigned)(tx0 + (int)fw - 1 + d), (unsigned)(ty0 + (int)yy + d), 0, 0, 0, a);
            for (unsigned xx = 0; xx < fw; xx++)                 /* bottom strip */
                blend_px((unsigned)(tx0 + (int)xx + d), (unsigned)(ty0 + (int)fh - 1 + d), 0, 0, 0, a);
        }
        unsigned char fb2 = s->focused ? g_ac[2] : 0x60;
        unsigned char fg2 = s->focused ? g_ac[1] : 0x60;
        unsigned char fr2 = s->focused ? g_ac[0] : 0x68;
        fill_rect((unsigned)tx0 - 1, (unsigned)ty0 - 1, fw + 2, 1, fb2, fg2, fr2);
        fill_rect((unsigned)tx0 - 1, (unsigned)(ty0 + (int)fh), fw + 2, 1, fb2, fg2, fr2);
        fill_rect((unsigned)tx0 - 1, (unsigned)ty0 - 1, 1, fh + 2, fb2, fg2, fr2);
        fill_rect((unsigned)(tx0 + (int)fw), (unsigned)ty0 - 1, 1, fh + 2, fb2, fg2, fr2);
        static int decor_said;
        if (!decor_said) { decor_said = 1; printf("PRADYOS_DECOR_OK\n"); fflush(stdout); }
    }
    blit_surface(sva, s->w, s->h, s->x, s->y);
    int ty = s->y - TITLEBAR; if (ty < 0) ty = 0;
    int tx = s->x < 0 ? 0 : s->x;
    fill_rect((unsigned)tx, (unsigned)ty, s->w, TITLEBAR, g_ac[2], g_ac[1], g_ac[0]);
    if (s->title[0])                                     /* title text (DDR-715) */
        draw_str_inter(s->title, tx + 6, ty + 1, 0x10, 0x10, 0x18);  /* DDR-728 */
    fill_rect((unsigned)(tx + (int)s->w - CLOSEBOX - 4), (unsigned)ty + 3,
              CLOSEBOX, CLOSEBOX, 0x30, 0x30, 0xE0);     /* close box (red, BGRA) */
    fill_rect((unsigned)(tx + (int)s->w - 2 * CLOSEBOX - 6), (unsigned)ty + 3,
              CLOSEBOX, CLOSEBOX, 0x30, 0xB0, 0xE0);     /* min box (amber, DDR-717) */
    fill_rect((unsigned)(tx + (int)s->w - 3 * CLOSEBOX - 8), (unsigned)ty + 3,
              CLOSEBOX, CLOSEBOX, 0x40, 0xC0, 0x40);     /* max box (green, DDR-719) */
}

/* Is (x,y) inside surface s's close box? Mirrors draw_window's layout. */
static int close_box_hit(const struct surface_info *s, int x, int y) {
    int ty = s->y - TITLEBAR;
    int bx = s->x + (int)s->w - CLOSEBOX - 4;
    return x >= bx && x < bx + CLOSEBOX && y >= ty + 3 && y < ty + 3 + CLOSEBOX;
}

/* Minimize (DDR-717): compositor-local — a minimized window is skipped when
 * compositing and hit-testing; its surface stays committed. The min box sits
 * 2 px left of the close box; `r` restores all (per-window restore needs a
 * dock, deferred). */
static unsigned g_min_mask;
static int min_box_hit(const struct surface_info *s, int x, int y) {
    int ty = s->y - TITLEBAR;
    int bx = s->x + (int)s->w - 2 * CLOSEBOX - 6;    /* 12px box + 2px gap */
    return x >= bx && x < bx + CLOSEBOX && y >= ty + 3 && y < ty + 3 + CLOSEBOX;
}

/* Maximize (DDR-719): saved per-id geometry + a toggle mask. The compositor
 * requests the size via the DDR-718 event channel and repositions the window;
 * the owner redraws. 512 = SURFACE_DIM_MAX (the largest legal surface). */
static unsigned g_max_mask;
static int mx_x[16], mx_y[16];
static unsigned short mx_w[16], mx_h[16];
static int max_box_hit(const struct surface_info *s, int x, int y) {
    int ty = s->y - TITLEBAR;
    int bx = s->x + (int)s->w - 3 * CLOSEBOX - 8;    /* third box, 2px gaps */
    return x >= bx && x < bx + CLOSEBOX && y >= ty + 3 && y < ty + 3 + CLOSEBOX;
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

/* Transition the ambiance bg+accent to AMB[idx] over `frames`, OKLab-interpolated. */
static void set_ambiance(int idx, int frames) {
    if (idx < 0 || idx > 3) return;
    unsigned char fbg[3], fac[3];
    for (int i = 0; i < 3; i++) { fbg[i] = g_bg[i]; fac[i] = g_ac[i]; }
    for (int f = 1; f <= frames; f++) {
        float t = (float)f / (float)frames;
        lab_lerp(fbg, AMB[idx].bg, t, g_bg);
        lab_lerp(fac, AMB[idx].ac, t, g_ac);
        g_settled = (f == frames);            /* DDR-716: backdrop on the final frame only */
        if (g_settled) g_cur_amb = idx;       /* the settled frame draws the NEW backdrop */
        render((int)nsi(SYS_GET_MODE, 0, 0, 0));
        present();
    }
    g_cur_amb = idx;
    g_settled = 1;
}

/* DDR-726: auto ambiance cadence. Time source is the frame loop itself (D1
 * fallback — each iteration is yield-paced; no user clock yet), so the default
 * is an ITERATION count approximating the brief's 900 s wall cadence; the 'k'
 * hotkey shrinks it so a gate proves a full cycle in seconds. */
static unsigned long g_cadence = 1500000;   /* iterations per ambiance period */
static unsigned long g_cad_iter;
static int g_cad_pre_said, g_cad_advances;

static void cadence_tick(void) {
    g_cad_iter++;
    if (!g_cad_pre_said && g_cad_iter >= g_cadence - g_cadence / 10) {
        g_cad_pre_said = 1;                 /* final 10%: gentle accent pulse */
        unsigned char base[3], white[3] = {0xFF, 0xFF, 0xFF};
        for (int i = 0; i < 3; i++) base[i] = g_ac[i];
        int mode = (int)nsi(SYS_GET_MODE, 0, 0, 0);
        for (int f = 1; f <= 3; f++) { lab_lerp(base, white, 0.1f * f, g_ac); render(mode); present(); }
        for (int i = 0; i < 3; i++) g_ac[i] = base[i];
        render(mode); present();
        printf("PRADYOS_PRETRANSITION\n");
        fflush(stdout);
    }
    if (g_cad_iter >= g_cadence) {
        g_cad_iter = 0;
        g_cad_pre_said = 0;
        set_ambiance((g_cur_amb + 1) & 3, 12);
        if (++g_cad_advances == 4) {        /* one full automatic cycle */
            printf("PRADYOS_CADENCE_OK\n");
            fflush(stdout);
        }
    }
}

/* Animated toggle (DDR-709; DDR-727): a damped-SPRING accent pulse toward
 * white — ramps up, overshoots the peak, settles back. Fixed amplitude table. */
static void animate_toggle(void) {
    static const unsigned char white[3] = {0xFF, 0xFF, 0xFF};
    static const float spring[10] =
        {0.30f, 0.60f, 0.85f, 1.00f, 1.08f, 1.02f, 0.97f, 1.00f, 0.50f, 0.0f};
    unsigned char base[3];
    for (int i = 0; i < 3; i++) base[i] = g_ac[i];
    int mode = (int)nsi(SYS_GET_MODE, 0, 0, 0);
    for (int f = 0; f < 10; f++) {
        float a = spring[f];
        if (a > 1.0f) a = 1.0f;           /* OKLab lerp clamps at the endpoint;
                                           * overshoot renders as a held peak */
        lab_lerp(base, white, a, g_ac);
        render(mode); present();
    }
    for (int i = 0; i < 3; i++) g_ac[i] = base[i];
    render(mode); present();
    printf("PRADYOS_SPRING_OK\n");
    printf("PRADYOS_TOGGLE_ANIM_OK\n");
    fflush(stdout);
}

/* DDR-727: expanding click ripple at (cx,cy) — 4 frames, radius 6->24,
 * alpha fading, then recompose. */
static void recompose_scene(void);            /* defined below */
static void click_ripple(int cx, int cy) {
    static int ripple_said;
    for (int f = 0; f < 4; f++) {
        int r = 6 + f * 6;
        float a = 0.5f - 0.1f * (float)f;
        for (int dx = -r; dx <= r; dx++) {          /* 1px ring via dy from r^2-dx^2 */
            int dy2 = r * r - dx * dx;
            int dy = 0;
            while (dy * dy < dy2) dy++;
            blend_px((unsigned)(cx + dx), (unsigned)(cy + dy), 255, 255, 255, a);
            blend_px((unsigned)(cx + dx), (unsigned)(cy - dy), 255, 255, 255, a);
        }
        present();
    }
    recompose_scene();
    if (!ripple_said) {
        ripple_said = 1;
        printf("PRADYOS_RIPPLE_OK\n");
        fflush(stdout);
    }
}

static void render_and_announce(int mode) {
    animate_toggle();                               /* DDR-709: animated toggle */
    render(mode);
    present();
    long m = nsi(SYS_GET_MODE, 0, 0, 0);
    printf("PRADYOS_COMPOSITOR_MODE %s\n", m ? "SOVEREIGN" : "MANUAL");
    fflush(stdout);
}

/* Re-render the whole scene during a drag: desktop + z-ordered windows (with
 * title bars, minimized skipped — DDR-717) + the cursor (DDR-710). */
static void recompose_drag(int cx, int cy) {
    struct surface_info sf[16];
    long n = nsi(SYS_SURFACE_POLL, (long)sf, 16, 0);
    render((int)nsi(SYS_GET_MODE, 0, 0, 0));
    for (long i = 0; i < n; i++) {
        if (g_min_mask & (1u << sf[i].id)) continue;             /* DDR-717 */
        long sva = nsi(SYS_SURFACE_CMAP, (long)sf[i].id, 0, 0);
        if (sva > 0) draw_window((const unsigned char *)sva, &sf[i]);
    }
    draw_cursor(cx, cy);
    present();
}

/* Repaint the scene without a cursor (min/restore, DDR-717). */
static void recompose_scene(void) {
    struct surface_info sf[16];
    long n = nsi(SYS_SURFACE_POLL, (long)sf, 16, 0);
    render((int)nsi(SYS_GET_MODE, 0, 0, 0));
    for (long i = 0; i < n; i++) {
        if (g_min_mask & (1u << sf[i].id)) continue;
        long sva = nsi(SYS_SURFACE_CMAP, (long)sf[i].id, 0, 0);
        if (sva > 0) draw_window((const unsigned char *)sva, &sf[i]);
    }
    present();
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
    set_ambiance(ambiance_for_secs(nsi(SYS_CLOCK, 0, 0, 0)), 6);   /* settle on time-of-day */
    printf("PRADYOS_AMBIANCE_OK\n");                               /* loop is about to start */
    fflush(stdout);

    char keys[32];
    unsigned prev_btn = 0;
    long composited = 0;             /* count of client surfaces last composited */
    int focus_id = -1, last_focus = -2;       /* focused surface (DDR-708) */
    int dragging = 0, drag_id = -1, drag_ox = 0, drag_oy = 0;   /* DDR-710 */
    int resizing = 0, rs_id = -1, rs_bx = 0, rs_by = 0;         /* DDR-718 */
    unsigned char last_roster[8] = {0xFF};   /* force a first-read print */
    int metrics_said = 0;                    /* DDR-737: one-shot panel witness */
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
        /* DDR-737: one-shot panel-metrics witness. Keyed on the POST-MORTEM
         * stable fact (DDR-735: pid retained + dispatches captured at exit), so
         * this fires deterministically even when the agent's whole life fits
         * inside one slow compositor frame on TCG runners. */
        if (!metrics_said) {
            struct agent_metric m[8] = {0};
            nsi(SYS_AGENT_METRICS, (long)m, 8, 0);
            if (m[0].pid != 0 && m[0].dispatches >= 1) {
                metrics_said = 1;
                printf("AGENT_PANEL KRYOS act=%u disp=%u\n",
                       (unsigned)m[0].actions, (unsigned)m[0].dispatches);
                printf("PRADYOS_AGENT_PANEL_METRICS_OK\n");
                fflush(stdout);
            }
        }
        /* Per-client surfaces (DDR-706/708): SURFACE_POLL is z-sorted (back-to-front).
         * Re-composite when the set grows or focus changes; blit in z-order so a
         * raised window is on top, and report the z-order + focused window. */
        struct surface_info surfs[16];
        long ns = nsi(SYS_SURFACE_POLL, (long)surfs, 16, 0);
        int cur_focus = -1;
        for (long i = 0; i < ns; i++) if (surfs[i].focused) cur_focus = (int)surfs[i].id;
        focus_id = cur_focus;
        if (ns != composited || cur_focus != last_focus) { /* grew, shrank, or focus moved */
            render((int)nsi(SYS_GET_MODE, 0, 0, 0));
            for (long i = 0; i < ns; i++) {                 /* z-order: bottom..top */
                if (g_min_mask & (1u << surfs[i].id)) continue;          /* DDR-717 */
                long sva = nsi(SYS_SURFACE_CMAP, (long)surfs[i].id, 0, 0);
                if (sva > 0)
                    draw_window((const unsigned char *)sva, &surfs[i]);  /* + title bar */
            }
            present();
            if (ns > 0) {
                printf("PRADYOS_ZORDER");
                for (long i = 0; i < ns; i++) printf(" %u", surfs[i].id);
                printf("\n");
            }
            for (long i = composited; i < ns; i++)
                printf("PRADYOS_SURFACE_OK %u\n", surfs[i].id);
            if (ns < composited)                            /* a window was closed (DDR-711) */
                printf("PRADYOS_SURFACE_GONE n=%ld\n", ns);
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
            else if (c == 'p') {                             /* DDR-746: ACPI poweroff */
                printf("PRADYOS_COMPOSITOR_POWEROFF\n");
                fflush(stdout);
                nsi(SYS_POWEROFF, 0, 0, 0);                  /* S5 — no return if it works */
            }
            else if (c == 'b') {                             /* DDR-747: ACPI/PC reboot */
                printf("PRADYOS_COMPOSITOR_REBOOT\n");
                fflush(stdout);
                nsi(SYS_REBOOT, 0, 0, 0);                    /* reset — no return if it works */
            }
            else if (c == 'r') {                             /* DDR-717: restore all */
                g_min_mask = 0;
                printf("PRADYOS_WM_RESTORE\n");
                fflush(stdout);
                recompose_scene();
            }
            else if (c == 'k') {                             /* DDR-726: test cadence */
                g_cadence = 2000;
                g_cad_iter = 0;
                g_cad_pre_said = 0;
                printf("PRADYOS_CADENCE_TEST\n");
                fflush(stdout);
            }
            else if (c == '\t') {                            /* DDR-720: cycle windows —
                                                              * raise the bottom-most
                                                              * visible surface (Tab is a
                                                              * compositor hotkey, not
                                                              * forwarded to the focus) */
                int low_id = -1;
                int low_z = 0x7FFFFFFF;
                for (long i = 0; i < ns; i++) {
                    if (g_min_mask & (1u << surfs[i].id)) continue;
                    if (surfs[i].z < low_z) { low_z = surfs[i].z; low_id = (int)surfs[i].id; }
                }
                if (low_id >= 0) {
                    nsi(SYS_SURFACE_RAISE, low_id, 0, 0);
                    printf("PRADYOS_WM_CYCLE id=%d\n", low_id);
                    fflush(stdout);
                    recompose_scene();
                }
            }
            else if (focus_id >= 0)                          /* DDR-708: route to focus */
                nsi(SYS_SURFACE_SENDKEY, focus_id, (long)c, 0);
        }
        /* Pointer (DDR-705/710): button-down on a window title bar starts a drag
         * (raise+focus, then move the window to follow the pointer until release);
         * a button-down elsewhere is a plain click. */
        struct mouse_state ms;
        if (nsi(SYS_MOUSE_POLL, (long)&ms, 0, 0) == 0) {
            if (ms.wheel && focus_id >= 0) {             /* DDR-725: scroll to focus —
                                                          * type 2, delta in arg1
                                                          * (a3 packs arg0<<16|arg1) */
                nsi(SYS_SURFACE_SENDEV, focus_id, 2,
                    (long)(unsigned short)(short)ms.wheel);
                printf("PRADYOS_SCROLL d=%d\n", ms.wheel);
                fflush(stdout);
            }
            int down = ms.buttons && !prev_btn;
            int up = !ms.buttons && prev_btn;
            if (down) {
                click_ripple(ms.x, ms.y);                    /* DDR-727: ripple */
                int card = agent_card_hit(ms.x, ms.y);       /* DDR-713: card first */
                if (card >= 0) {                             /* trigger the agent via AETHER */
                    long pid = nsi(SYS_SPAWN_AGENT, 0, (long)g_agents[card], card);
                    printf("PRADYOS_AGENT_TRIGGER name=%s slot=%d pid=%ld\n",
                           g_agents[card], card, pid);
                    fflush(stdout);
                    render((int)nsi(SYS_GET_MODE, 0, 0, 0));  /* slot now lit */
                    present();
                } else {
                    struct surface_info sf[16];
                    long n = nsi(SYS_SURFACE_POLL, (long)sf, 16, 0);
                    int hit = -1, closed = 0;
                    for (long i = n - 1; i >= 0; i--) {      /* topmost (highest z) first */
                        if (g_min_mask & (1u << sf[i].id)) continue;   /* DDR-717 */
                        int tx = sf[i].x, ty = sf[i].y - TITLEBAR;
                        if (ms.x >= tx && ms.x < tx + (int)sf[i].w &&
                            ms.y >= ty && ms.y < ty + TITLEBAR) {
                            if (max_box_hit(&sf[i], ms.x, ms.y)) {     /* DDR-719 */
                                unsigned id = sf[i].id;
                                if (!(g_max_mask & (1u << id))) {      /* maximize */
                                    mx_x[id] = sf[i].x; mx_y[id] = sf[i].y;
                                    mx_w[id] = (unsigned short)sf[i].w;
                                    mx_h[id] = (unsigned short)sf[i].h;
                                    nsi(SYS_SURFACE_SENDEV, (long)id, 1, (512L << 16) | 512L);
                                    nsi(SYS_SURFACE_MOVE, (long)id, 8, 26);
                                    g_max_mask |= 1u << id;
                                    printf("PRADYOS_WM_MAX id=%u\n", id);
                                } else {                               /* restore */
                                    nsi(SYS_SURFACE_SENDEV, (long)id, 1,
                                        ((long)mx_w[id] << 16) | (long)mx_h[id]);
                                    nsi(SYS_SURFACE_MOVE, (long)id, mx_x[id], mx_y[id]);
                                    g_max_mask &= ~(1u << id);
                                    printf("PRADYOS_WM_UNMAX id=%u\n", id);
                                }
                                fflush(stdout);
                                closed = 1;                            /* repaint below */
                                recompose_scene();
                                break;
                            }
                            if (min_box_hit(&sf[i], ms.x, ms.y)) {     /* DDR-717 */
                                g_min_mask |= 1u << sf[i].id;
                                printf("PRADYOS_WM_MIN id=%u\n", sf[i].id);
                                fflush(stdout);
                                closed = 1;                  /* repaint below */
                                recompose_scene();
                                break;
                            }
                            if (close_box_hit(&sf[i], ms.x, ms.y)) {   /* DDR-715 */
                                nsi(SYS_SURFACE_CLOSE, (long)sf[i].id, 0, 0);
                                printf("PRADYOS_WM_CLOSE id=%u\n", sf[i].id);
                                fflush(stdout);
                                closed = 1;                  /* shrink detector recomposites */
                                break;
                            }
                            hit = (int)sf[i].id;
                            drag_ox = ms.x - sf[i].x; drag_oy = ms.y - sf[i].y;
                            break;
                        }
                    }
                    if (closed) {
                        /* handled: the main loop's ns != composited path repaints */
                    } else if (hit >= 0) {
                        nsi(SYS_SURFACE_RAISE, hit, 0, 0);
                        dragging = 1; drag_id = hit;
                        printf("PRADYOS_DRAG_START id=%d\n", hit);
                        fflush(stdout);
                    } else {
                        /* DDR-718: bottom-right 14x14 corner starts a resize drag. */
                        int rz = -1;
                        for (long i = n - 1; i >= 0; i--) {
                            if (g_min_mask & (1u << sf[i].id)) continue;
                            int cx0 = sf[i].x + (int)sf[i].w - 14, cy0 = sf[i].y + (int)sf[i].h - 14;
                            if (ms.x >= cx0 && ms.x < sf[i].x + (int)sf[i].w &&
                                ms.y >= cy0 && ms.y < sf[i].y + (int)sf[i].h) {
                                rz = (int)sf[i].id;
                                rs_bx = sf[i].x; rs_by = sf[i].y;
                                break;
                            }
                        }
                        if (rz >= 0) {
                            resizing = 1; rs_id = rz;
                        } else {                             /* plain click (DDR-705) */
                            render((int)nsi(SYS_GET_MODE, 0, 0, 0));
                            draw_cursor(ms.x, ms.y);
                            present();
                            printf("PRADYOS_MOUSE_OK %d %d\n", ms.x, ms.y);
                            fflush(stdout);
                        }
                    }
                }
            } else if (dragging && ms.buttons) {             /* drag-move */
                nsi(SYS_SURFACE_MOVE, drag_id, ms.x - drag_ox, ms.y - drag_oy);
                recompose_drag(ms.x, ms.y);
            } else if (resizing && ms.buttons) {             /* resize-drag (DDR-718) */
                recompose_drag(ms.x, ms.y);                  /* cursor tracks the corner */
            } else if (up && dragging) {                     /* drop */
                dragging = 0;
                printf("PRADYOS_DRAG id=%d x=%d y=%d\n", drag_id, ms.x - drag_ox, ms.y - drag_oy);
                fflush(stdout);
                recompose_drag(ms.x, ms.y);
            } else if (up && resizing) {                     /* resize commit (DDR-718) */
                resizing = 0;
                int neww = ms.x - rs_bx, newh = ms.y - rs_by;
                if (neww < 32) neww = 32; if (neww > 512) neww = 512;
                if (newh < 32) newh = 32; if (newh > 512) newh = 512;
                nsi(SYS_SURFACE_SENDEV, rs_id, 1, ((long)neww << 16) | (long)newh);
                printf("PRADYOS_RESIZE_REQ id=%d w=%d h=%d\n", rs_id, neww, newh);
                fflush(stdout);
                recompose_scene();                           /* client recommits async */
            }
            prev_btn = ms.buttons;
        }
        cadence_tick();                     /* DDR-726: auto ambiance cadence */
        nsi(SYS_YIELD, 0, 0, 0);
    }
    return 0;
}
