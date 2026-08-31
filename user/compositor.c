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
#define SYS_EXECVE      14          /* DDR-1027: Ctrl+Alt+T spawns /TERM.ELF */
#define SYS_FORK        15
#define SYS_FB_INFO     43
#define SYS_FB_MAP      44
#define SYS_FB_FLUSH    45
#define SYS_INPUT_POLL  46
#define SYS_KEY_POLL    96          /* DDR-991: structured key events */

/* DDR-991 ABI — must match kernel/drivers/input/ps2kbd.h. */
#define KMOD_CTRL  0x02u          /* DDR-1027: Ctrl+Alt+T terminal */
#define KMOD_ALT   0x04u          /* DDR-995: Alt+Tab window cycling */
#define KMOD_META  0x08u
#define KEY_TAB    0x09u
struct key_ev { unsigned char code, mods, down, ascii; };

/* DDR-1027: how many terminals this compositor has launched. */
static int g_terms;

/* DDR-1028: has the first successful SYS_MOUSE_POLL been announced? */
static int g_input_said;

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
/* DDR-998: `gen` APPENDED. This struct is declared TWICE — here and at
 * kernel/syscall/sys_surface.c — and SYS_SURFACE_POLL copies out
 * count * sizeof(struct surface_info) into THIS array, so a size disagreement
 * silently overruns the buffer below rather than failing to build. Both
 * declarations must change in the SAME commit (§INV.13's PT_HI lesson). */
struct surface_info { unsigned id, w, h; int x, y, z; unsigned focused; char title[16];
                      unsigned gen; };
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

/* DDR-1029 instrument: where do the first loop iterations spend their time?
 *
 * DDR-1028 measured a ~10 s gap between PRADYOS_AMBIANCE_OK and this
 * compositor's FIRST SYS_MOUSE_POLL, and named it as the common cause behind
 * DDR-1025, DDR-1026 and an intermittent smoke-wmclose -- but it did not
 * establish WHERE the time goes, so §NON-NEGOTIABLE 3 forbade a fix. mpoll
 * climbing 2 -> 161 -> 767 -> 1678 across successive heartbeats says the loop is
 * running and its early iterations are enormously slow, then accelerate.
 *
 * SYS_CLOCK is seconds, which is coarse -- and it is the right resolution here
 * precisely because the thing being measured is ~10 s. A finer clock would add
 * a vDSO dependency to answer a question whole seconds already answer.
 *
 * Bounded to the first LOOPSTAMP_ITERS iterations so this cannot become a
 * per-frame print: an unconditional stamp would emit thousands of lines a
 * second and slow the very loop it measures, which is the mistake DDR-941's
 * on-change PRADYOS_BTN_STATE rule exists to prevent. */
#define LOOPSTAMP_ITERS 3
static int g_loop_iter;
static void loopstamp(const char *phase) {
    if (g_loop_iter > LOOPSTAMP_ITERS)
        return;
    printf("PRADYOS_LOOPSTAMP i=%d at=%s s=%ld\n",
           g_loop_iter, phase, nsi(SYS_CLOCK, 0, 0, 0));
    fflush(stdout);
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

/* DDR-910: screen pixel -> virtio-tablet absolute (0..32767). The scaling lives
 * HERE, in the only code that knows the framebuffer size, so a gate never has to
 * duplicate the mapping and get it wrong a second way. */
/* DDR-911: which surface ids have already been told they were composited. */
static unsigned g_composited_told;

static int tab_x(int px) {
    unsigned w = g_fi.width > 1 ? g_fi.width - 1 : 1;
    if (px < 0) px = 0;
    return (int)((long)px * 32767L / (long)w);
}

static int tab_y(int py) {
    unsigned h = g_fi.height > 1 ? g_fi.height - 1 : 1;
    if (py < 0) py = 0;
    return (int)((long)py * 32767L / (long)h);
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

/* DDR-1012: a full-width horizon band, centred on row cy, alpha falling off
 * quadratically from peak_a at the centre to 0 at cy +/- half. Same shape as
 * radial_glow's falloff and the same blend_px primitive, in one axis.
 *
 * Cost is 2*half*width blends -- ~90k at half=44 on a 1024-wide screen, i.e.
 * CHEAPER than one radial (a 300px disc is ~280k). It inherits DDR-716 D3's
 * guard: backdrops draw only on settled frames, never mid-OKLab-lerp. */
/* DDR-1012 §4: read one pixel back out of the framebuffer, packed RGB, so the
 * compositor can publish what it actually DREW rather than what it intended.
 * A gate that only greps a sentinel tests a printf -- the vacuity trap DDR-973 §6
 * and DDR-1008 each caught once. */
static unsigned fb_sample(unsigned x, unsigned y) {
    if (!g_fb || x >= g_fi.width || y >= g_fi.height) return 0;
    const unsigned char *p = g_fb + (unsigned long)y * g_fi.stride + (unsigned long)x * 4;
    return ((unsigned)p[2] << 16) | ((unsigned)p[1] << 8) | (unsigned)p[0];
}

static unsigned g_hz_pre, g_hz_post;    /* DDR-1012 §4: the band's own witness */
static void horizon_band(int cy, int half,
                         unsigned char cr, unsigned char cg, unsigned char cb, float peak_a) {
    /* Sample the centre pixel BEFORE and AFTER, and publish both.
     *
     * The first version of this gate compared the band centre against a row
     * above the band. That is VACUOUS: render() lays a per-row vertical gradient
     * (DDR-723), so two different rows differ whether or not a band was drawn --
     * the assertion would have passed on a no-op band. Same pixel, before and
     * after, is the only comparison that isolates THIS draw. Caught by reading
     * the assertion against render()'s gradient, before the gate was ever run. */
    g_hz_pre = fb_sample((unsigned)(g_fi.width / 2), (unsigned)cy);
    int y0 = cy - half, y1 = cy + half;
    if (y0 < 0) y0 = 0;
    if (y1 > (int)g_fi.height) y1 = (int)g_fi.height;
    float h2 = (float)half * (float)half;
    for (int y = y0; y < y1; y++) {
        float dy = (float)(y - cy);
        float a = peak_a * (1.0f - (dy * dy) / h2);
        if (a <= 0.0f) continue;
        for (unsigned x = 0; x < g_fi.width; x++)
            blend_px(x, (unsigned)y, cb, cg, cr, a);
    }
    /* AFTER the band and before any caller draws over it -- DUSK's sun-bloom is
     * laid down next and would otherwise contaminate the reading. */
    g_hz_post = fb_sample((unsigned)(g_fi.width / 2), (unsigned)cy);
}

/* The settled per-ambiance backdrop (DDR-716, brief §1). Drawn only on settled
 * frames (not mid-OKLab-lerp — D3's perf guard); announces each ambiance's
 * first settled render and PRADYOS_BACKDROP_OK once all four have been seen. */
static int g_settled = 1;                 /* cleared during ambiance transitions */
static void render_backdrop(void) {
    unsigned w = g_fi.width, h = g_fi.height;
    switch (g_cur_amb) {
    case 0:                                        /* DAWN: motes + horizon band */
        /* DDR-1012: DAWN was the ONLY ambiance with no backdrop at all -- this
         * arm was a bare `break`. The rose band at 62% is its signature. */
        horizon_band((int)(h * 62 / 100), 44, 0xFF, 0xA8, 0xC0, 0.18f);
        break;
    case 1:                                        /* DAY: 3-node gradient mesh */
        radial_glow((int)(w / 4),     (int)(h / 4),     (int)(w * 30 / 100), 0x8F, 0xC8, 0xFF, 0.20f);
        radial_glow((int)(w / 2),     (int)(h * 2 / 3), (int)(w * 25 / 100), 0xFF, 0xFF, 0xFF, 0.12f);
        radial_glow((int)(w * 3 / 4), (int)(h / 3),     (int)(w * 28 / 100), 0x1E, 0x50, 0xA0, 0.18f);
        break;
    case 2:                                        /* DUSK: horizon band + sun-bloom */
        /* DDR-1012: the band goes down FIRST, at 88%, so the bloom centred at
         * 90% rises out of it instead of floating above nothing. */
        horizon_band((int)(h * 88 / 100), 40, 0xFF, 0x9A, 0x3C, 0.22f);
        radial_glow((int)(w * 85 / 100), (int)(h * 90 / 100), (int)(w * 35 / 100), 0xFF, 0x78, 0x1E, 0.25f);
        break;
    default:                                       /* NIGHT: two nebulas */
        radial_glow((int)(w * 30 / 100), (int)(h * 40 / 100), (int)(w * 600 / 1024), 0x12, 0x00, 0x24, 0.30f);
        radial_glow((int)(w * 70 / 100), (int)(h * 60 / 100), (int)(w * 500 / 1024), 0x00, 0x12, 0x20, 0.30f);
        break;
    }
    /* DDR-1012 §4: publish what was DRAWN, not that drawing was attempted.
     * `in` samples the band centre, `out` samples 8 px clear of its top edge --
     * ABOVE, deliberately: below the band DUSK's sun-bloom overlaps and would
     * move the reference. The gate asserts in != out, which a no-op band fails. */
    static unsigned char hz_said[4];
    static int hz_ok_said;
    if ((g_cur_amb == 0 || g_cur_amb == 2) && !hz_said[g_cur_amb]) {
        int hcy   = (g_cur_amb == 0) ? (int)(h * 62 / 100) : (int)(h * 88 / 100);
        int hhalf = (g_cur_amb == 0) ? 44 : 40;
        hz_said[g_cur_amb] = 1;
        (void)hhalf;
        printf("PRADYOS_HORIZON %s y=%d pre=%06X post=%06X\n",
               AMB[g_cur_amb].name, hcy, g_hz_pre, g_hz_post);
        if (!hz_ok_said && hz_said[0] && hz_said[2]) {
            hz_ok_said = 1;
            printf("PRADYOS_HORIZON_OK\n");
        }
        fflush(stdout);
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
static int g_manual_announced;   /* DDR-893: Manual sentinels print once */

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
/* ---- DDR-893 (item 39): MANUAL MODE is a DIFFERENT DESKTOP -----------------
 *
 * Before this, "manual" changed one string. The item is explicit that a mode
 * flag on the Sovereign layout is not the feature, and it is right: the two
 * modes exist to answer different questions. Sovereign says "what are my agents
 * doing" — an ambient gradient, a particle field, glass cards for eight agents.
 * Manual says "let me drive" — and a user driving wants chrome where a
 * traditional desktop puts it, not ambience.
 *
 * So Manual is structurally different, not restyled:
 *
 *   - FLAT background. No gradient, no particles, no backdrop. Those are
 *     Sovereign's ambient language and they cost a full-screen per-row fill and
 *     a particle pass every frame for a user who is trying to read a window.
 *   - A TASKBAR along the BOTTOM with a start button and window buttons — the
 *     conventional position and the conventional affordance.
 *   - A MENU BAR along the top instead of Sovereign's accent stripe.
 *   - NO agent panel. Agents keep running; Manual simply does not put them on
 *     screen, because the panel is the Sovereign answer to the Sovereign
 *     question.
 *
 * The two render paths share only put_px/fill_rect/draw_str — the primitives.
 * Sharing the LAYOUT would be the mode-flag design the item rules out.
 */
#define MANUAL_TASKBAR_H 28
#define MANUAL_MENUBAR_H 18

static void render_manual(void) {
    unsigned W = g_fi.width, H = g_fi.height;
    if (!W || !H)
        return;

    /* Flat desktop fill. Deliberately one fill, not a per-row gradient. */
    fill_rect(0, 0, W, H, (unsigned char)(g_bg[2] * 0.85f),
                          (unsigned char)(g_bg[1] * 0.85f),
                          (unsigned char)(g_bg[0] * 0.85f));

    /* Top menu bar. */
    fill_rect(0, 0, W, MANUAL_MENUBAR_H, 40, 40, 44);
    draw_str("File  Edit  View  Window  Help", 8, 4, 1, 220, 220, 226);

    /* Bottom taskbar with a start button, then one button per window slot. */
    unsigned ty = (H > MANUAL_TASKBAR_H) ? H - MANUAL_TASKBAR_H : 0;
    fill_rect(0, ty, W, MANUAL_TASKBAR_H, 32, 32, 36);
    fill_rect(0, ty, W, 1, g_ac[2], g_ac[1], g_ac[0]);      /* 1px accent edge */

    fill_rect(6, ty + 5, 72, MANUAL_TASKBAR_H - 10, g_ac[2], g_ac[1], g_ac[0]);
    draw_str("START", 16, ty + 9, 1, 20, 20, 24);

    for (int i = 0; i < 4; i++) {
        int bx = 88 + i * 104;
        if ((unsigned)(bx + 96) >= W)
            break;
        fill_rect((unsigned)bx, ty + 5, 96, MANUAL_TASKBAR_H - 10, 58, 58, 64);
        draw_str("Window", (unsigned)bx + 8, ty + 9, 1, 210, 210, 216);
    }

    draw_str("MANUAL MODE", 24, MANUAL_MENUBAR_H + 10, 2,
             g_ac[2], g_ac[1], g_ac[0]);

    g_frame++;
    if (!g_manual_announced) {
        /* Sentinels naming the STRUCTURE, so a gate can tell the two layouts
         * apart rather than reading a title string that either could print. */
        printf("PRADYOS_MANUAL_TASKBAR_OK h=%d\n", MANUAL_TASKBAR_H);
        printf("PRADYOS_MANUAL_MENUBAR_OK h=%d\n", MANUAL_MENUBAR_H);
        printf("PRADYOS_MANUAL_NO_AGENT_PANEL\n");
        fflush(stdout);
        g_manual_announced = 1;
    }
}

static void render(int mode) {
    if (!mode) {                 /* DDR-893: Manual is its own desktop */
        render_manual();
        return;
    }
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
    draw_str("SOVEREIGN MODE", 24, 24, 3,
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

/* DDR-997: resize handles on every edge, not just the bottom-right corner.
 * 14 px is DDR-718's corner size, kept so the hit target does not change size
 * between the old handle and the new ones. */
/* DDR-998: ask the owner to close, then force. Both clocks must expire.
 * Seconds alone are too coarse (a click just before a boundary would grant a
 * grace of nearly zero); frames alone are scheduling-dependent, which is what
 * DDR-911 removed from surfacetest.c after item 16 changed it. */
#define SURF_EV_CLOSE        4
#define CLOSE_GRACE_SECS     2
#define CLOSE_GRACE_FRAMES   64
#define CLOSE_PENDING_MAX    8

#define RZBAND    14
#define RZ_N      0x1
#define RZ_S      0x2
#define RZ_W      0x4
#define RZ_E      0x8
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

/* DDR-1007: the work area — the part of the screen a window may occupy.
 *
 * "Maximize at real display size" cannot mean the whole framebuffer, because the
 * compositor draws chrome a maximized window must not cover, and the chrome
 * DIFFERS BY MODE. DDR-893 made Manual a structurally different desktop rather
 * than a restyle, so this has to branch on the mode or it is wrong in one of
 * them:
 *
 *   Sovereign (mode != 0): 6 px accent bar on top; agent panel occupying the
 *                          right 210 px (render_agent_panel / agent_card_hit).
 *   Manual    (mode == 0): MANUAL_MENUBAR_H on top, MANUAL_TASKBAR_H at the
 *                          bottom, and NO agent panel.
 *
 * Returns the area in framebuffer coordinates. Callers place window CONTENT
 * TITLEBAR px below ay, because draw_window puts the title bar at s->y - TITLEBAR. */
#define WA_MARGIN 8
static void work_area(int mode, int *ax, int *ay, int *aw, int *ah) {
    int W = (int)g_fi.width, H = (int)g_fi.height;
    int top, bot, right;
    if (mode) {                                  /* Sovereign */
        top   = 6;                               /* accent bar (render()) */
        bot   = H;
        right = (g_fi.width >= 220) ? W - 210 : W;   /* mirrors render_agent_panel */
    } else {                                     /* Manual */
        top   = MANUAL_MENUBAR_H;
        bot   = H - MANUAL_TASKBAR_H;
        right = W;
    }
    *ax = WA_MARGIN;
    *ay = top + WA_MARGIN;
    *aw = right - *ax - WA_MARGIN;
    *ah = bot   - *ay - WA_MARGIN;
    if (*aw < 32) *aw = 32;
    if (*ah < 32) *ah = 32;
}

/* The largest surface the compositor will ever ask for, in one axis. Clamped to
 * SURFACE_DIM_MAX so a larger scanout degrades to the biggest legal surface
 * instead of getting -EINVAL from sys_surface_resize. */
#define SURFACE_DIM_MAX 1024                     /* MUST track sys_surface.c:17 */
static int wa_clamp(int v) {
    if (v < 32) return 32;
    if (v > SURFACE_DIM_MAX) return SURFACE_DIM_MAX;
    return v;
}

/* Maximize (DDR-719): saved per-id geometry + a toggle mask. The compositor
 * requests the size via the DDR-718 event channel and repositions the window;
 * the owner redraws. DDR-1007: the size is now the work area, not a hardcoded
 * 512 -- that constant was SURFACE_DIM_MAX, which covered 26% of a 1024x768
 * screen. */
static unsigned g_max_mask;
static int mx_x[16], mx_y[16];
static unsigned short mx_w[16], mx_h[16];
static int max_box_hit(const struct surface_info *s, int x, int y) {
    int ty = s->y - TITLEBAR;
    int bx = s->x + (int)s->w - 3 * CLOSEBOX - 8;    /* third box, 2px gaps */
    return x >= bx && x < bx + CLOSEBOX && y >= ty + 3 && y < ty + 3 + CLOSEBOX;
}

/* ---- DDR-1008: the dock -- per-window restore -------------------------------
 *
 * DDR-717 shipped minimize complete on the HIDE side and left restore as one
 * keystroke: `r` clears the whole mask. A user with three windows who minimizes
 * one and wants it back has to un-minimize all three, and until they do there is
 * nothing on screen saying the window still exists.
 *
 * The dock is a strip of tiles along the bottom, one per minimized window,
 * drawn OVER the windows and present only while g_min_mask != 0.
 *
 * Three decisions, each with a cheaper wrong version:
 *
 *  - Tiles are ordered by ASCENDING SURFACE ID, not by z-order. SURFACE_POLL
 *    returns z-sorted and z changes on every raise, so poll order would
 *    reshuffle the dock when an unrelated window is clicked -- bad UI, and an
 *    untestable target. DDR-910's finding was exactly that a gate silently
 *    encoding window order broke when fair-share scheduling changed it.
 *
 *  - The dock is an OVERLAY and does not shrink DDR-1007's work area. If it did,
 *    a maximized window would resize itself whenever an unrelated window was
 *    minimized.
 *
 *  - SOVEREIGN ONLY. Manual already draws window buttons in its own taskbar
 *    (render_manual), so a second strip above the first would be two docks.
 *    Wiring those existing buttons to this same restore path is the Manual
 *    answer and is a separate change -- recorded as not done, not skipped. */
#define DOCK_TILE_W  96
#define DOCK_TILE_H  24
#define DOCK_GAP     4
#define DOCK_MARGIN  8

static int dock_tile_x(int slot) { return DOCK_MARGIN + slot * (DOCK_TILE_W + DOCK_GAP); }
static int dock_tile_y(void)     { return (int)g_fi.height - DOCK_TILE_H - DOCK_MARGIN; }

/* Fill `out` with the ids of minimized surfaces in ascending-id order; returns
 * the count. Ids come from the live poll, so a minimized surface that has since
 * been destroyed contributes no tile. */
static int dock_ids(const struct surface_info *sf, long n, unsigned *out, int max) {
    int cnt = 0;
    for (unsigned id = 0; id < 16 && cnt < max; id++) {
        if (!(g_min_mask & (1u << id))) continue;
        for (long i = 0; i < n; i++)
            if (sf[i].id == id) { out[cnt++] = id; break; }
    }
    return cnt;
}

static void draw_dock(const struct surface_info *sf, long n) {
    if (!g_min_mask) return;
    unsigned ids[16];
    int cnt = dock_ids(sf, n, ids, 16);
    int ty = dock_tile_y();
    for (int k = 0; k < cnt; k++) {
        int tx = dock_tile_x(k);
        if (tx + DOCK_TILE_W > (int)g_fi.width) break;      /* dock is full */
        glass_card((unsigned)tx, (unsigned)ty, DOCK_TILE_W, DOCK_TILE_H);
        for (long i = 0; i < n; i++)
            if (sf[i].id == ids[k] && sf[i].title[0])
                draw_str(sf[i].title, tx + 8, ty + 9, 1, 0xE0, 0xE0, 0xF0);
    }
}

/* Which dock tile is under (x,y), or -1. Mirrors draw_dock's layout exactly --
 * one expression per coordinate, shared with the renderer through dock_tile_x /
 * dock_tile_y so the clickable target cannot drift from the drawn one. */
static int dock_hit(const struct surface_info *sf, long n, int x, int y) {
    if (!g_min_mask) return -1;
    unsigned ids[16];
    int cnt = dock_ids(sf, n, ids, 16);
    int ty = dock_tile_y();
    if (y < ty || y >= ty + DOCK_TILE_H) return -1;
    for (int k = 0; k < cnt; k++) {
        int tx = dock_tile_x(k);
        if (tx + DOCK_TILE_W > (int)g_fi.width) break;   /* same cut draw_dock makes */
        if (x >= tx && x < tx + DOCK_TILE_W)
            return (int)ids[k];
    }
    return -1;
}

/* DDR-1008 §3: publish tile centres in TABLET coordinates (§INV.5), latched on
 * g_min_mask so the log carries one burst per change rather than one per frame.
 *
 * This CANNOT be folded into the block that emits PRADYOS_WM_GEOM. That block is
 * guarded by `ns != composited || cur_focus != last_focus || geom_moved`, and
 * minimizing changes none of the three -- not the surface count, not focus, not
 * any surface's x/y/w/h. A dock line emitted only from there would never appear
 * after a minimize, which is the only moment it matters. Found by reading the
 * republish condition, not by a failing run.
 *
 * `n=` repeats on every line deliberately: it is what lets a gate assert that
 * restoring one window left the others minimized, which is the whole difference
 * between this and DDR-717's restore-all. */
static unsigned g_dock_pub_mask = 0xFFFFu;      /* force a first publish */
static void publish_dock(const struct surface_info *sf, long n) {
    if (g_min_mask == g_dock_pub_mask) return;
    g_dock_pub_mask = g_min_mask;
    unsigned ids[16];
    int cnt = dock_ids(sf, n, ids, 16);
    int ty = dock_tile_y();
    if (cnt == 0) {
        printf("PRADYOS_WM_DOCK n=0\n");
        fflush(stdout);
        return;
    }
    for (int k = 0; k < cnt; k++) {
        if (dock_tile_x(k) + DOCK_TILE_W > (int)g_fi.width) break;  /* undrawn */
        const char *t = "-";
        for (long i = 0; i < n; i++)
            if (sf[i].id == ids[k] && sf[i].title[0]) t = sf[i].title;
        printf("PRADYOS_WM_DOCK n=%d id=%u title=%s tile=%d,%d\n",
               cnt, ids[k], t,
               tab_x(dock_tile_x(k) + DOCK_TILE_W / 2),
               tab_y(ty + DOCK_TILE_H / 2));
    }
    fflush(stdout);
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

/* DDR-726 + DDR-895: auto ambiance cadence, now driven by a CLOCK.
 *
 * The original comment here said the time source was "the frame loop itself …
 * an ITERATION count approximating the brief's 900 s wall cadence", because no
 * user clock was available when DDR-726 was written. That measured how much CPU
 * the compositor was GIVEN, not elapsed time, so smoke-cadence failed under CI
 * load whenever the loop did not accumulate enough iterations (CI 31843212987).
 * It is the same anti-pattern DDR-911 fixed in surfacetest.c.
 *
 * A user clock does exist now: the vDSO page is mapped read-only into every
 * address space and carries wall_time_ns, readable with a plain load and ZERO
 * syscalls (IMP-C) — the same source DDR-915 used to pace the actiondag
 * rendezvous. That keeps this hot loop syscall-free, which is why an iteration
 * count was chosen in the first place. */
#define VDSO_USER_VA 0x00007FFFFFF00000ull
static unsigned long long vdso_ns(void) {
    return *(volatile unsigned long long *)VDSO_USER_VA;
}

/* Period per ambiance, in NANOSECONDS. 900 s is the brief's cadence, stated
 * directly instead of as "1 500 000 iterations, which is about 900 s if the
 * compositor gets a typical share of a typical CPU". */
static unsigned long long g_cadence_ns = 900ULL * 1000ULL * 1000ULL * 1000ULL;
static unsigned long long g_cad_start_ns;   /* 0 = not yet armed */
static int g_cad_pre_said, g_cad_advances;

static void cadence_tick(void) {
    unsigned long long now = vdso_ns();
    if (!g_cad_start_ns)
        g_cad_start_ns = now;               /* arm on the first frame */
    unsigned long long g_cad_iter = now - g_cad_start_ns;   /* elapsed, ns */
    unsigned long long g_cadence = g_cadence_ns;
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
        g_cad_start_ns = now;               /* DDR-895: restart the period here */
        g_cad_pre_said = 0;
        /* DDR-965: under the test knob the ANIMATION, not the cadence, is the
         * floor. cadence_tick() runs once per FRAME, so an advance cannot
         * complete in less than the transition it renders — ~16 render+present
         * pairs, measured at ~11.3 s against a 2000 ms target. CI reached only
         * n=3 of the 4 the gate needs. Shrink the transition when the test
         * cadence is armed: the animation still happens, g_settled still lands
         * on the final frame (DDR-716), and only the frame COUNT changes.
         * Derived from g_cadence_ns rather than a new flag — no new writable
         * global (DDR-826). The knob is used by smoke-cadence alone. */
        int cad_test = g_cadence_ns < 60ULL * 1000ULL * 1000ULL * 1000ULL;
        set_ambiance((g_cur_amb + 1) & 3, cad_test ? 2 : 12);
        ++g_cad_advances;
        /* smoke-cadence instrument. That gate fails as "no full auto cycle" —
         * CADENCE_OK missing — while CADENCE_TEST and PRETRANSITION both pass,
         * so the knob armed and at least one period nearly completed. The clock
         * is therefore advancing and the question is only the RATE: how many
         * advances happened, and how long each actually took against the 2 s
         * test target.
         *
         * cadence_tick() runs once per FRAME, so an advance cannot be observed
         * sooner than the next frame. elapsed_ms >> target_ms means the
         * compositor is not being scheduled frames often enough to close four
         * periods inside the gate's window — a starvation reading, not a clock
         * one. elapsed_ms ~= target_ms with n < 4 means it simply ran out of
         * wall time. Four lines per run at most -- DDR-970: the bound is now
         * ENFORCED; advances continue past 4 in a 180 s run and the printf used
         * to fire on every one, so the comment was false and the log grew
         * without bound. ring-3 printf+fflush is one kwrite, so this cannot be
         * spliced (DDR-963 §4). */
        if (g_cad_advances <= 4) {
            printf("PRADYOS_CAD_ADV n=%d elapsed_ms=%llu target_ms=%llu\n",
                   g_cad_advances, g_cad_iter / 1000000ULL, g_cadence / 1000000ULL);
            fflush(stdout);
        }
        if (g_cad_advances == 4) {          /* one full automatic cycle */
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
    draw_dock(sf, n);                    /* DDR-1008: overlay, above the windows */
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
    draw_dock(sf, n);                    /* DDR-1008: overlay, above the windows */
    present();
    publish_dock(sf, n);                 /* DDR-1008 §3: latched on g_min_mask */
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
    loopstamp("pre-ambiance");        /* DDR-1029: five full-screen renders follow */
    for (int k = 0; k < 4; k++) {
        set_ambiance(k, 6);
        printf("PRADYOS_AMBIANCE %s\n", AMB[k].name);
        fflush(stdout);
        loopstamp(AMB[k].name);       /* DDR-1029: cost of ONE ambiance render */
    }
    set_ambiance(ambiance_for_secs(nsi(SYS_CLOCK, 0, 0, 0)), 6);   /* settle on time-of-day */
    loopstamp("post-ambiance");
    printf("PRADYOS_AMBIANCE_OK\n");                               /* loop is about to start */
    fflush(stdout);

    char keys[32];
    unsigned prev_btn = 0;
    long composited = 0;             /* count of client surfaces last composited */
    int focus_id = -1, last_focus = -2;       /* focused surface (DDR-708) */
    int dragging = 0, drag_id = -1, drag_ox = 0, drag_oy = 0;   /* DDR-710 */
    int resizing = 0, rs_id = -1, rs_bx = 0, rs_by = 0;         /* DDR-718 */
    /* DDR-997: the drag's ORIGINAL geometry plus which edges it grabbed.
     * A W or N drag derives both the new size and the new origin from the
     * edge that must hold still, so w0/h0 are needed at commit, not just
     * the origin DDR-718 recorded. */
    int rs_w0 = 0, rs_h0 = 0, rs_edge = 0;
    /* DDR-997 §9.8: the press point, and a one-shot witness that the
     * compositor has OBSERVED the pointer somewhere else. SYS_MOUSE_POLL
     * reads current state rather than an event queue (DDR-941), so a gate
     * that releases after a fixed sleep is betting the compositor polled
     * in between. In CI it did not, and every arm committed at its own
     * START coordinate. One line per drag turns that bet into a
     * precondition the injector can wait for — DDR-910's rule, applied to
     * the drag phase instead of the click. */
    int rs_px = 0, rs_py = 0, rs_moved = 0;
    unsigned char last_roster[8] = {0xFF};   /* force a first-read print */
    int metrics_said = 0;                    /* DDR-737: one-shot panel witness */
    /* DDR-968: the witness above is gated on pid!=0 && dispatches>=1, and when
     * smoke-agents goes red in CI neither value is anywhere in the log. These
     * two locals (no new writable global, DDR-826) bound a periodic print of
     * that predicate: 24 lines max, 128 frames apart. A green boot arms the
     * witness in the first frames and emits at most one. */
    unsigned wit_frames = 0, wit_said = 0;
    /* DDR-997: the geometry block below was republished only when the surface
     * COUNT or the focus changed, so after a move or a resize the last
     * PRADYOS_WM_GEOM line described a window that had since moved. That was
     * survivable while the only gate reading it (smoke-evresize) did one drag
     * and stopped; smoke-resizeall drives four in a row and needs the handles
     * refreshed between them. Keep the last published rect per slot and treat a
     * change as a publish trigger — exact, not hashed, because a hash collision
     * here would silently republish nothing. */
    /* DDR-998: close requests awaiting the owner. (id, gen) not id alone — a
     * slot freed inside the grace can be re-taken by a DIFFERENT process, and
     * forcing on id alone would destroy a window that merely inherited the
     * number (§NON-NEGOTIABLE 18, one level up). */
    int  cp_id[CLOSE_PENDING_MAX];
    unsigned cp_gen[CLOSE_PENDING_MAX];
    long cp_secs[CLOSE_PENDING_MAX];
    unsigned cp_frame[CLOSE_PENDING_MAX];
    int  cp_n = 0;
    unsigned cp_frames = 0;               /* compositor frames, for the floor */
    short pub_x[16], pub_y[16];
    unsigned short pub_w[16], pub_h[16];
    for (int i = 0; i < 16; i++) {          /* impossible values: force pass 1 */
        pub_x[i] = pub_y[i] = -32768;
        pub_w[i] = pub_h[i] = 0xFFFF;
    }
    for (;;) {
        g_loop_iter++;
        loopstamp("top");
        /* DDR-709: real-time sun-driven ambiance — transition at hour boundaries. */
        int amb = ambiance_for_secs(nsi(SYS_CLOCK, 0, 0, 0));
        if (amb != g_cur_amb) set_ambiance(amb, 8);
        /* Named-agent panel (DDR-707): when AETHER's roster changes, re-render
         * (the panel is part of render()). SYS_AGENT_ROSTER is instantaneous
         * liveness by design (DDR-730: a card self-corrects to inactive when its
         * agent dies), so it drives the CARD ONLY — the serial roster report is
         * emitted from the post-mortem-stable plane below (DDR-914), because a
         * whole agent lifecycle can fit between two frames on a TCG runner. */
        unsigned char roster[8] = {0};
        nsi(SYS_AGENT_ROSTER, (long)roster, 8, 0);
        int changed = 0;
        for (int i = 0; i < 8; i++) if (roster[i] != last_roster[i]) changed = 1;
        if (changed) {
            render((int)nsi(SYS_GET_MODE, 0, 0, 0));
            present();
            /* PRADYOS_AGENTS_OK keeps its ORIGINAL meaning — "the compositor's
             * roster loop is live" — because smoke-agent-click (Makefile:1662)
             * uses it as the readiness trigger for its mouse injector. Only the
             * per-slot AGENT lines move to the stable plane (DDR-914); moving
             * this one too would delay that click past its 120s bound. */
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
                /* DDR-914: the serial roster witness answers "does this slot
                 * hold a spawned agent?" (pid is retained past exit) rather
                 * than "is it alive this instant?". Gated behind dispatches>=1,
                 * so it still cannot print unless the kernel provably scheduled
                 * the agent — strictly stronger than a lucky live sample. */
                for (int i = 0; i < 8; i++)
                    printf("AGENT %s %s\n", g_agents[i],
                           m[i].pid != 0 ? "active" : "inactive");
                printf("AGENT_PANEL KRYOS act=%u disp=%u\n",
                       (unsigned)m[0].actions, (unsigned)m[0].dispatches);
                printf("PRADYOS_AGENT_PANEL_METRICS_OK\n");
                fflush(stdout);
            } else if (wit_said < 24 && (wit_frames++ & 127) == 0) {
                /* DDR-968: say WHICH term is holding the witness down. Printed
                 * on a cadence rather than on change, because a frozen
                 * predicate is the signal and one line then silence reads the
                 * same as a stopped loop — the rising n= separates them. */
                wit_said++;
                printf("PRADYOS_AGENT_WITNESS_WAIT pid=%u disp=%u state=%u n=%u\n",
                       (unsigned)m[0].pid, (unsigned)m[0].dispatches,
                       (unsigned)m[0].state, wit_frames);
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
        int geom_moved = 0;                            /* DDR-997 */
        for (long i = 0; i < ns && i < 16; i++) {
            if (pub_x[i] != (short)surfs[i].x || pub_y[i] != (short)surfs[i].y ||
                pub_w[i] != (unsigned short)surfs[i].w ||
                pub_h[i] != (unsigned short)surfs[i].h) {
                pub_x[i] = (short)surfs[i].x;  pub_y[i] = (short)surfs[i].y;
                pub_w[i] = (unsigned short)surfs[i].w;
                pub_h[i] = (unsigned short)surfs[i].h;
                geom_moved = 1;
            }
        }
        if (ns != composited || cur_focus != last_focus || geom_moved) { /* grew, shrank, moved, or focus */
            render((int)nsi(SYS_GET_MODE, 0, 0, 0));
            for (long i = 0; i < ns; i++) {                 /* z-order: bottom..top */
                if (g_min_mask & (1u << surfs[i].id)) continue;          /* DDR-717 */
                long sva = nsi(SYS_SURFACE_CMAP, (long)surfs[i].id, 0, 0);
                if (sva > 0) {
                    draw_window((const unsigned char *)sva, &surfs[i]);  /* + title bar */
                    /* DDR-911: tell the owner, once, that this surface has
                     * actually been composited. A client that needs to know its
                     * window became visible must be TOLD, not left to infer it
                     * from elapsed time or loop iterations — those are
                     * scheduling-dependent and item 16 changed them. */
                    if (!(g_composited_told & (1u << surfs[i].id))) {
                        g_composited_told |= (1u << surfs[i].id);
                        nsi(SYS_SURFACE_SENDEV, (long)surfs[i].id, 3, 0);
                    }
                }
            }
            draw_dock(surfs, ns);        /* DDR-1008: overlay, above the windows */
            present();
            publish_dock(surfs, ns);     /* DDR-1008 §3 */
            if (ns > 0) {
                printf("PRADYOS_ZORDER");
                for (long i = 0; i < ns; i++) printf(" %u", surfs[i].id);
                printf("\n");
            }
            /* DDR-910: publish the close/min box centres in TABLET coordinates, so a
             * gate clicks what the compositor actually drew rather than a hardcoded
             * pixel. These are the SAME expressions draw_window uses for the boxes
             * themselves — one source of truth, so the emitted target cannot drift
             * from what the hit-test accepts. A gate that hardcoded coordinates was
             * depending on window creation order, which fair-share scheduling is
             * free to change (item 16). */
            for (long gi = 0; gi < ns; gi++) {
                int gty = surfs[gi].y - TITLEBAR; if (gty < 0) gty = 0;
                int gtx = surfs[gi].x < 0 ? 0 : surfs[gi].x;
                int gcx = gtx + (int)surfs[gi].w - CLOSEBOX - 4 + CLOSEBOX / 2;
                int gmx = gtx + (int)surfs[gi].w - 2 * CLOSEBOX - 6 + CLOSEBOX / 2;
                int gby = gty + 3 + CLOSEBOX / 2;
                /* DDR-894: also publish the RESIZE corner. Same expression as the
                 * DDR-718 hit-test (bottom-right 14x14 box at x+w-14 .. x+w),
                 * so the emitted target cannot drift from what the hit-test
                 * accepts — the centre is (x+w-7, y+h-7). smoke-evresize used to
                 * hardcode absolute coordinates for that 14-PIXEL target, which
                 * is why it flaked whenever the window moved at all. */
                int grx = gtx + (int)surfs[gi].w - 7;
                int gry = surfs[gi].y + (int)surfs[gi].h - 7;
                /* DDR-897: also publish a DRAG point on the title bar. The drag
                 * hit is the fallback AFTER the close/min/max boxes, and those
                 * sit on the RIGHT — the leftmost is the max box at
                 * x + w - 3*CLOSEBOX - 8. So the drag strip is x .. that edge,
                 * and the safe point is its MIDPOINT. Deriving it from the same
                 * box expression matters: a fixed offset does not work, because
                 * these windows are narrow (w~64 leaves the boxes occupying
                 * x+20..x+64, so a naive x+20 lands ON the max box — measured,
                 * 0/3 pass). smoke-drag hardcoded SX/SY and was 0/3 before this. */
                int gmaxbox = gtx + (int)surfs[gi].w - 3 * CLOSEBOX - 8;
                int gdx = gtx + (gmaxbox - gtx) / 2;
                int gdy = gty + TITLEBAR / 2;
                /* DDR-983: publish the MAX box too. `gmaxbox` above is already
                 * the exact `bx` from max_box_hit() (x + w - 3*CLOSEBOX - 8),
                 * and gby is the shared box-centre y that close/min use — so
                 * the emitted target cannot drift from what the hit-test
                 * accepts, which is the property DDR-894 established for rz=.
                 * It was computed here for the drag point's left edge and never
                 * published, which is why smoke-wmmax still hardcoded pixels
                 * against §INV.5 while wmclose did not.
                 * APPENDED, never inserted: drag_inject.sh isolates fields by
                 * name (${geom##*dg=}) and mouse_inject.sh scans tokens with
                 * startswith(field + "="), so a trailing field is safe for both;
                 * "mx=" also cannot prefix-collide with "min=". */
                /* DDR-997 §5: publish the seven NEW resize handles the same
                 * way rz= publishes the SE corner — one field per handle, each
                 * the CENTRE of its hit region, derived from the same
                 * expressions the hit-test uses so the emitted target cannot
                 * drift from what is accepted. `rz=` keeps meaning SE, so every
                 * existing parser is untouched.
                 * APPENDED, never inserted, for the reason recorded above; and
                 * none of the new names contains the substring "rz=" (rzn=,
                 * rzs=, rzw=, rze=, rznw=, rzne=, rzsw= all break it), so
                 * drag_inject.sh's ${geom##*rz=} still isolates the SE field. */
                int gw = (int)surfs[gi].w, gh = (int)surfs[gi].h;
                int gy0 = surfs[gi].y;
                int gmidx = gtx + gw / 2, gmidy = gy0 + gh / 2;
                int gl = gtx + RZBAND / 2, gr = gtx + gw - RZBAND / 2;
                int gt = gy0 + RZBAND / 2, gb2 = gy0 + gh - RZBAND / 2;
                printf("PRADYOS_WM_GEOM id=%u title=%s close=%d,%d min=%d,%d rz=%d,%d dg=%d,%d mx=%d,%d "
                       "rzn=%d,%d rzs=%d,%d rzw=%d,%d rze=%d,%d "
                       "rznw=%d,%d rzne=%d,%d rzsw=%d,%d\n",
                       surfs[gi].id, surfs[gi].title[0] ? surfs[gi].title : "-",
                       tab_x(gcx), tab_y(gby), tab_x(gmx), tab_y(gby),
                       tab_x(grx), tab_y(gry), tab_x(gdx), tab_y(gdy),
                       tab_x(gmaxbox + CLOSEBOX / 2), tab_y(gby),
                       tab_x(gmidx), tab_y(gt),   tab_x(gmidx), tab_y(gb2),
                       tab_x(gl),    tab_y(gmidy), tab_x(gr),   tab_y(gmidy),
                       tab_x(gl),    tab_y(gt),   tab_x(gr),    tab_y(gt),
                       tab_x(gl),    tab_y(gb2));
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
        /* DDR-992: Super+M — a physical sovereign-mode TOGGLE. Read the chord
         * stream before the byte stream: the driver no longer emits text for a
         * non-Shift chord (DDR-992 §2), so these are disjoint and Super+M can
         * no longer be undone by the plain-'m' branch below. */
        loopstamp("pre-keys");
        {
            struct key_ev kev[16];
            long ne = nsi(SYS_KEY_POLL, (long)kev, 16, 0);
            for (long i = 0; i < ne; i++) {
                if (!kev[i].down)
                    continue;
                /* DDR-995: Alt+Tab cycles windows. DDR-720 bound this to a
                 * BARE Tab on the byte stream, which meant no application on
                 * this system could ever receive a Tab character — the branch
                 * was unconditional and terminal, so it never reached the focus
                 * routing below. The byte stream carries no modifier state, so
                 * the chord could not be told from the keystroke until DDR-991
                 * added this ring; DDR-992 then stopped a non-Shift chord from
                 * emitting text at all, which makes the two cases disjoint at
                 * the source rather than merely distinguishable here. */
                if (kev[i].code == KEY_TAB && (kev[i].mods & KMOD_ALT)) {
                    int low_id = -1;
                    int low_z = 0x7FFFFFFF;
                    for (long k = 0; k < ns; k++) {
                        if (g_min_mask & (1u << surfs[k].id)) continue;   /* DDR-717 */
                        if (surfs[k].z < low_z) { low_z = surfs[k].z; low_id = (int)surfs[k].id; }
                    }
                    if (low_id >= 0) {
                        nsi(SYS_SURFACE_RAISE, low_id, 0, 0);
                        printf("PRADYOS_WM_CYCLE id=%d\n", low_id);
                        fflush(stdout);
                        recompose_scene();
                    }
                }
                /* DDR-1027: Ctrl+Alt+T launches a PRISM terminal window.
                 * fork+execve, NOT SYS_SPAWN_AGENT: that is the AETHER roster
                 * path and would consume a fixed roster slot, mint agent
                 * capabilities, and make a terminal show up in
                 * SYS_AGENT_ROSTER as an autonomous agent. A terminal is an
                 * application; fork+execve is the door PRISM's own `run` uses.
                 *
                 * Read from the DDR-991 event ring for the reason DDR-995
                 * records: the byte stream carries no modifier state. DDR-992
                 * went further and stopped a non-Shift chord emitting text at
                 * all, so Ctrl+Alt+T produces no 't' byte and nothing on this
                 * system loses the letter t. */
                if (kev[i].code == 't') {
                    /* Reported for EVERY 't' press, chord or not, with the
                     * modifier byte and whether it spawned. A gate that only
                     * saw successful spawns could not tell "Ctrl+Alt+T works"
                     * from "any T spawns a terminal" -- and it cannot recover
                     * that from spawn COUNTS either, because input_inject.sh
                     * replays its whole key list four times and the cap below
                     * clamps the total. This line makes the discrimination
                     * itself observable: a spawn=1 whose mods lack KMOD_CTRL is
                     * the defect, named. */
                    int spawned = 0;
                    if ((kev[i].mods & KMOD_CTRL) && (kev[i].mods & KMOD_ALT)) {
                        /* Bounded so a stuck key cannot fork the machine
                         * flat. The gate does reach this cap: the injector
                         * replays its list four times, and four Ctrl+Alt+T
                         * presses are four terminals, which is what a user
                         * pressing it four times should get. */
                        if (g_terms < 4) {
                            long tp = nsi(SYS_FORK, 0, 0, 0);
                            if (tp == 0) {
                                nsi(SYS_EXECVE, (long)"/TERM.ELF", 0, 0);
                                nsi(SYS_EXIT, 127, 0, 0);
                            }
                            if (tp > 0) {
                                g_terms++;
                                spawned = 1;
                                printf("PRADYOS_TERM_SPAWN pid=%ld n=%d\n", tp, g_terms);
                                fflush(stdout);
                            }
                        }
                    }
                    printf("PRADYOS_TERM_CHORD mods=%u spawn=%d\n",
                           (unsigned)kev[i].mods, spawned);
                    fflush(stdout);
                }
                if (kev[i].code == 'm' && (kev[i].mods & KMOD_META)) {
                    int cur = (int)nsi(SYS_GET_MODE, 0, 0, 0);
                    int nxt = cur ? 0 : 1;
                    nsi(SYS_SET_MODE, nxt, 0, 0);
                    printf("PRADYOS_SUPERKEY_TOGGLE from=%d to=%d\n", cur, nxt);
                    fflush(stdout);
                    render_and_announce(nxt);
                }
            }
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
                /* DDR-895: a short period in the SAME units (ns), not an
                 * iteration count. 2 s per ambiance => a full 4-ambiance cycle
                 * in ~8 s, comfortably inside the gate window even on a loaded
                 * TCG runner, because it is now elapsed time rather than
                 * accumulated CPU share. */
                g_cadence_ns = 2ULL * 1000ULL * 1000ULL * 1000ULL;
                g_cad_start_ns = vdso_ns();  /* re-arm from now */
                g_cad_pre_said = 0;
                printf("PRADYOS_CADENCE_TEST\n");
                fflush(stdout);
            }
            else if (focus_id >= 0)                          /* DDR-708: route to focus */
                nsi(SYS_SURFACE_SENDKEY, focus_id, (long)c, 0);
        }
        /* Pointer (DDR-705/710): button-down on a window title bar starts a drag
         * (raise+focus, then move the window to follow the pointer until release);
         * a button-down elsewhere is a plain click. */
        loopstamp("pre-mouse");
        struct mouse_state ms;
        if (nsi(SYS_MOUSE_POLL, (long)&ms, 0, 0) == 0) {
            /* DDR-1028: the FIRST successful pointer poll, announced once.
             *
             * Every pointer gate's injector waits for PRADYOS_AMBIANCE_OK and
             * then starts clicking. That sentinel is printed at compositor.c:1184
             * -- "loop is about to start" -- and it does NOT mean the pointer is
             * being serviced. Measured on smoke-wmclose: ambiance at t=5500 and
             * mpoll STILL 0 at t=6000, with the first poll at t=6500. Ring 3 had
             * not read the pointer once in the first 60 s of guest time, so a
             * full second of injected clicks went into a compositor that was not
             * looking. smoke-mouse survives that only because DDR-1026's latch
             * holds the press until someone finally polls; smoke-wmclose cannot,
             * because its target self-closes inside the gap.
             *
             * This is the honest readiness signal: it is printed from inside the
             * branch that just read the pointer, so it cannot be true early. */
            if (!g_input_said) {
                g_input_said = 1;
                printf("PRADYOS_INPUT_READY\n");
                fflush(stdout);
            }
            if (ms.wheel && focus_id >= 0) {             /* DDR-725: scroll to focus —
                                                          * type 2, delta in arg1
                                                          * (a3 packs arg0<<16|arg1) */
                nsi(SYS_SURFACE_SENDEV, focus_id, 2,
                    (long)(unsigned short)(short)ms.wheel);
                printf("PRADYOS_SCROLL d=%d\n", ms.wheel);
                fflush(stdout);
            }
            /* DDR-941 (mode B): log the button state ONLY when it changes.
             * SYS_MOUSE_POLL reads current state and returns 0 on every call
             * (sys_input.c:39-45) — it is not an event queue — so this block
             * runs every iteration of the main loop. An unconditional print
             * here would emit thousands of lines per second, swamp the serial
             * log, and slow the compositor enough to mask the ~11% flake it is
             * meant to catch. On-change keeps every transition and no spam. */
            if (ms.buttons != prev_btn) {
                printf("PRADYOS_BTN_STATE buttons=%u prev=%u x=%d y=%d\n",
                       (unsigned)ms.buttons, (unsigned)prev_btn, ms.x, ms.y);
                fflush(stdout);
            }
            int down = ms.buttons && !prev_btn;
            int up = !ms.buttons && prev_btn;
            if (down) {
                click_ripple(ms.x, ms.y);                    /* DDR-727: ripple */
                /* DDR-1008: the dock is drawn OVER the windows and the agent
                 * panel, so it must be hit-tested first or a tile that is
                 * visibly on top would be unclickable wherever it overlaps. */
                int dock_id = -1;
                if (g_min_mask) {
                    struct surface_info dsf[16];
                    long dn = nsi(SYS_SURFACE_POLL, (long)dsf, 16, 0);
                    dock_id = dock_hit(dsf, dn, ms.x, ms.y);
                    if (dock_id >= 0) {
                        /* Clear ONLY this window's bit. `g_min_mask = 0` here
                         * would be DDR-717's restore-all wearing this feature's
                         * name -- and would pass a gate that only checks the
                         * clicked window came back. See DDR-1008 §4. */
                        g_min_mask &= ~(1u << (unsigned)dock_id);
                        nsi(SYS_SURFACE_RAISE, (long)dock_id, 0, 0);   /* raise + focus */
                        /* NOT "PRADYOS_WM_RESTORE_ONE": DDR-717 already prints
                         * PRADYOS_WM_RESTORE, and `grep -q PRADYOS_WM_RESTORE`
                         * in smoke-wmmin would match that as a PREFIX. Named
                         * after UNMAX instead, which collides with nothing. */
                        printf("PRADYOS_WM_UNMIN id=%d\n", dock_id);
                        fflush(stdout);
                        recompose_scene();
                    }
                }
                int card = (dock_id >= 0) ? -1 : agent_card_hit(ms.x, ms.y);  /* DDR-713 */
                if (dock_id >= 0) {
                    /* handled above; fall through to the frame's tail */
                } else if (card >= 0) {                             /* trigger the agent via AETHER */
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
                                    /* DDR-1007: fill the work area. The CONTENT
                                     * origin is TITLEBAR below the area top,
                                     * because the title bar is drawn above y. */
                                    int ax, ay, aw, ah;
                                    work_area((int)nsi(SYS_GET_MODE, 0, 0, 0),
                                              &ax, &ay, &aw, &ah);
                                    int mw = wa_clamp(aw);
                                    int mh = wa_clamp(ah - TITLEBAR);
                                    nsi(SYS_SURFACE_SENDEV, (long)id, 1,
                                        ((long)mw << 16) | (long)mh);
                                    nsi(SYS_SURFACE_MOVE, (long)id, ax, ay + TITLEBAR);
                                    g_max_mask |= 1u << id;
                                    /* DDR-1007 §5: publish the size we ASKED
                                     * for, so the gate asserts the client's ack
                                     * against it instead of against a constant
                                     * the gate already knew (§INV.5). */
                                    printf("PRADYOS_WM_MAX id=%u w=%d h=%d\n", id, mw, mh);
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
                                /* DDR-998: ASK first. The owner gets a bounded
                                 * grace to flush state and close itself; the
                                 * retire pass below forces it if it does not.
                                 * Authority is unchanged — the owner may delay
                                 * within the grace, never veto. */
                                int dup = 0;
                                for (int q = 0; q < cp_n; q++)
                                    if (cp_id[q] == (int)sf[i].id) dup = 1;
                                if (!dup && cp_n < CLOSE_PENDING_MAX) {
                                    nsi(SYS_SURFACE_SENDEV, (long)sf[i].id,
                                        SURF_EV_CLOSE, 0);
                                    cp_id[cp_n]    = (int)sf[i].id;
                                    cp_gen[cp_n]   = sf[i].gen;
                                    cp_secs[cp_n]  = nsi(SYS_CLOCK, 0, 0, 0);
                                    cp_frame[cp_n] = cp_frames;
                                    cp_n++;
                                    printf("PRADYOS_WM_CLOSE_REQ id=%u gen=%u\n",
                                           sf[i].id, sf[i].gen);
                                    fflush(stdout);
                                } else if (!dup) {
                                    /* Table full: fall back to DDR-715's
                                     * immediate close rather than ignoring the
                                     * click. A desktop that stops responding to
                                     * its close box is worse than a discourteous
                                     * one. */
                                    nsi(SYS_SURFACE_CLOSE, (long)sf[i].id, 0, 0);
                                    printf("PRADYOS_WM_CLOSE id=%u owner=0 full=1\n",
                                           sf[i].id);
                                    fflush(stdout);
                                    closed = 1;
                                }
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
                        /* DDR-718: bottom-right 14x14 corner starts a resize drag.
                         * DDR-997: so does any other edge. Eight regions, all
                         * RZBAND px thick — N/S/E/W strips and the four corners
                         * where two strips overlap. SE is unchanged bit-for-bit
                         * (RZ_S|RZ_E reduces to exactly the old predicate), which
                         * matters because it is the one path with a green gate.
                         * This runs only when the title-bar loop above found no
                         * hit, so move keeps priority over resize on an ambiguous
                         * pixel (DDR-997 §2) — the M3 mutation is that ordering. */
                        int rz = -1;
                        for (long i = n - 1; i >= 0; i--) {
                            if (g_min_mask & (1u << sf[i].id)) continue;
                            int rx = sf[i].x, ry = sf[i].y;
                            int rw = (int)sf[i].w, rh = (int)sf[i].h;
                            if (ms.x < rx || ms.x >= rx + rw ||
                                ms.y < ry || ms.y >= ry + rh) continue;
                            int m = 0;
                            if (ms.x <  rx + RZBAND)      m |= RZ_W;
                            if (ms.x >= rx + rw - RZBAND) m |= RZ_E;
                            if (ms.y <  ry + RZBAND)      m |= RZ_N;
                            if (ms.y >= ry + rh - RZBAND) m |= RZ_S;
                            /* A window narrower than 2*RZBAND has overlapping
                             * strips; take the nearer edge rather than both,
                             * which would otherwise pin the size to the clamp. */
                            if ((m & RZ_W) && (m & RZ_E))
                                m &= ~((ms.x - rx < rw / 2) ? RZ_E : RZ_W);
                            if ((m & RZ_N) && (m & RZ_S))
                                m &= ~((ms.y - ry < rh / 2) ? RZ_S : RZ_N);
                            if (!m) continue;                /* interior: plain click */
                            rz = (int)sf[i].id;
                            rs_bx = rx; rs_by = ry;
                            rs_w0 = rw; rs_h0 = rh;
                            rs_edge = m;
                            rs_px = ms.x; rs_py = ms.y; rs_moved = 0;
                            break;
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
                if (!rs_moved && (ms.x != rs_px || ms.y != rs_py)) {
                    rs_moved = 1;                            /* DDR-997 §9.8 */
                    printf("PRADYOS_RESIZE_TRACK id=%d x=%d y=%d\n", rs_id, ms.x, ms.y);
                    fflush(stdout);
                }
                recompose_drag(ms.x, ms.y);                  /* cursor tracks the corner */
            } else if (up && dragging) {                     /* drop */
                dragging = 0;
                printf("PRADYOS_DRAG id=%d x=%d y=%d\n", drag_id, ms.x - drag_ox, ms.y - drag_oy);
                fflush(stdout);
                recompose_drag(ms.x, ms.y);
            } else if (up && resizing) {                     /* resize commit (DDR-718) */
                resizing = 0;
                int x0 = rs_bx, y0 = rs_by, w0 = rs_w0, h0 = rs_h0;
                int newx = x0, newy = y0, neww = w0, newh = h0;
                /* DDR-997 §4: derive the SIZE from the edge that must hold
                 * still, clamp it, and only THEN place the origin. Clamping
                 * after deriving the origin leaves the origin where the
                 * unclamped drag put it, so a window dragged past the 32 px
                 * floor keeps sliding with its width pinned and the edge
                 * separates from the pointer. That is the M2 mutation. */
                if (rs_edge & RZ_E) neww = ms.x - x0;
                if (rs_edge & RZ_S) newh = ms.y - y0;
                if (rs_edge & RZ_W) neww = (x0 + w0) - ms.x;
                if (rs_edge & RZ_N) newh = (y0 + h0) - ms.y;
                /* DDR-1007 §4: same ceiling as maximize. Leaving this at 512
                 * would let a user maximize to a size they cannot then drag to. */
                neww = wa_clamp(neww); newh = wa_clamp(newh);
                if (rs_edge & RZ_W) newx = (x0 + w0) - neww;
                if (rs_edge & RZ_N) newy = (y0 + h0) - newh;
                /* DDR-997 §3: MOVE first, then resize. The two are separate
                 * syscalls and the pair is not atomic; move-then-resize leaves
                 * the window at its new position with its old size for one
                 * frame, which is the shape a plain move already produces.
                 * Resize-then-move would first grow AWAY from the pointer. */
                if (newx != x0 || newy != y0) {
                    nsi(SYS_SURFACE_MOVE, rs_id, newx, newy);
                    printf("PRADYOS_DRAG id=%d x=%d y=%d\n", rs_id, newx, newy);
                }
                nsi(SYS_SURFACE_SENDEV, rs_id, 1, ((long)neww << 16) | (long)newh);
                printf("PRADYOS_RESIZE_REQ id=%d w=%d h=%d\n", rs_id, neww, newh);
                /* Re-poll so the line below reports the surface's ACTUAL origin
                 * rather than the origin this code intended. Without this the
                 * M1 mutation — drop the MOVE, keep everything else — is
                 * invisible: newx is still computed and would still be printed,
                 * so the gate would pass on a window that never moved. That is
                 * the same decorative-arm mistake DDR-996's first arm B made.
                 * SYS_SURFACE_MOVE is synchronous, so x/y here are settled;
                 * w/h stay the REQUESTED values because the client honours the
                 * resize asynchronously (that round-trip is smoke-evresize's
                 * job). Mixing the two is deliberate and is exactly the
                 * property under test: the origin actually moved to must
                 * complement the width actually asked for. */
                struct surface_info chk[16];
                long cn = nsi(SYS_SURFACE_POLL, (long)chk, 16, 0);
                int obsx = -1, obsy = -1;      /* not found -> fails the assert */
                for (long ci = 0; ci < cn; ci++)
                    if ((int)chk[ci].id == rs_id) { obsx = chk[ci].x; obsy = chk[ci].y; }
                /* DDR-997 §6: the load-bearing assertion is the FIXED-EDGE
                 * equality (x+w == x0+w0 for a W drag), not "width changed" —
                 * a width-only check passes on a broken origin. Publish both
                 * the before and after geometry on one line so the gate can
                 * assert the invariant without reconstructing it from
                 * elsewhere. Separate sentinel, so RESIZE_REQ's existing
                 * parsers are untouched. */
                printf("PRADYOS_RESIZE_FIX id=%d edge=%d x0=%d y0=%d w0=%d h0=%d "
                       "x=%d y=%d w=%d h=%d\n",
                       rs_id, rs_edge, x0, y0, w0, h0, obsx, obsy, neww, newh);
                fflush(stdout);
                recompose_scene();                           /* client recommits async */
            }
            prev_btn = ms.buttons;
        }
        /* DDR-998: retire pending close requests. Three outcomes, and the third
         * is the one that needs the generation counter. */
        cp_frames++;
        for (int q = 0; q < cp_n; ) {
            int live = -1;
            for (long k = 0; k < ns; k++)
                if ((int)surfs[k].id == cp_id[q]) { live = (int)k; break; }
            int drop = 0;
            if (live < 0) {
                /* Gone: the owner honoured the request and closed itself. */
                printf("PRADYOS_WM_CLOSE id=%d owner=1\n", cp_id[q]);
                fflush(stdout);
                drop = 1;
            } else if (surfs[live].gen != cp_gen[q]) {
                /* The slot was freed and RE-TAKEN inside the grace. The window
                 * standing here now is a different tenancy that merely
                 * inherited the id; forcing on the id alone would destroy an
                 * innocent process's window. Drop the request and say so once —
                 * silence here would hide a real lifecycle event. */
                printf("PRADYOS_WM_CLOSE_STALE id=%d was=%u now=%u\n",
                       cp_id[q], cp_gen[q], surfs[live].gen);
                fflush(stdout);
                drop = 1;
            } else {
                long now = nsi(SYS_CLOCK, 0, 0, 0);
                long el  = now - cp_secs[q];
                if (el < 0) el += 24 * 3600;             /* midnight wrap */
                if (el >= CLOSE_GRACE_SECS &&
                    (cp_frames - cp_frame[q]) >= CLOSE_GRACE_FRAMES) {
                    nsi(SYS_SURFACE_CLOSE, (long)cp_id[q], 0, 0);
                    /* Denominator, not just a verdict (§NON-NEGOTIABLE 17):
                     * how much grace the owner actually burned without using. */
                    printf("PRADYOS_WM_CLOSE id=%d owner=0 secs=%ld frames=%u\n",
                           cp_id[q], el, cp_frames - cp_frame[q]);
                    fflush(stdout);
                    drop = 1;
                }
            }
            if (drop) {
                for (int r = q; r < cp_n - 1; r++) {
                    cp_id[r]  = cp_id[r + 1];  cp_gen[r]   = cp_gen[r + 1];
                    cp_secs[r] = cp_secs[r + 1]; cp_frame[r] = cp_frame[r + 1];
                }
                cp_n--;
            } else {
                q++;
            }
        }
        cadence_tick();                     /* DDR-726: auto ambiance cadence */
        nsi(SYS_YIELD, 0, 0, 0);
    }
    return 0;
}
