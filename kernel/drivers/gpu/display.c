/* kernel/drivers/gpu/display.c -- see display.h and DDR-1142. */
#include "display.h"
#include "virtio_gpu.h"
#include "boot_info.h"
#include "console.h"
#include "vmm.h"
#include "fwcfg.h"

/* Kernel VA window for the GOP framebuffer: a PML4 slot of its own beside the
 * ECAM (0xFFFFC0..), virtio BAR (0xFFFFD0..) and NVMe (0xFFFFD2..) windows. */
#define GOP_VBASE 0xFFFFD40000000000ull

/* Refuse geometry no real mode has; a block that passes magic+check but claims
 * a 1,000,000-pixel-wide panel is still not something to map. */
#define GOP_MAX_DIM 8192u

static struct boot_fb g_boot;          /* copied verbatim from 0x4FE0 */
static int      g_boot_state;          /* 0 none, 1 valid, 2 check failed */
static int      g_gop_used;
static uint8_t *g_gop_kva;             /* first pixel, kernel VA */
static uint64_t g_gop_phys;            /* first pixel, physical */

void display_capture_boot(void) {
    const struct boot_fb *src = (const struct boot_fb *)(uintptr_t)BOOT_FB_PHYS;
    g_boot = *src;
    if (g_boot.magic != BOOT_FB_MAGIC) {
        g_boot_state = 0;
        return;
    }
    uint32_t chk = g_boot.magic ^ g_boot.format ^ (uint32_t)g_boot.base ^
                   (uint32_t)(g_boot.base >> 32) ^ g_boot.width ^
                   g_boot.height ^ g_boot.stride_px;
    g_boot_state = (chk == g_boot.check) ? 1 : 2;
}

static int gop_geometry_sane(void) {
    return g_boot.width && g_boot.height &&
           g_boot.width <= GOP_MAX_DIM && g_boot.height <= GOP_MAX_DIM &&
           g_boot.stride_px >= g_boot.width && g_boot.stride_px <= GOP_MAX_DIM &&
           g_boot.base != 0;
}

/* Four quadrants, four colours, in the BGRX byte order the scanout reads. The
 * gate compares each quadrant centre against a QMP screendump -- what the
 * device is showing, not what this CPU wrote -- so a wrong base, a wrong stride
 * or a mapping that never reached the device all leave the firmware's own
 * screen in the dump. */
static void gop_pattern(void) {
    static const uint32_t q[4] = { 0x00FF0000u, 0x0000FF00u,    /* red, green   */
                                   0x000000FFu, 0x00FFFF00u };  /* blue, yellow */
    uint32_t w = g_boot.width, h = g_boot.height, sp = g_boot.stride_px;
    for (uint32_t y = 0; y < h; y++) {
        volatile uint32_t *row = (volatile uint32_t *)(g_gop_kva + (uint64_t)y * sp * 4u);
        uint32_t qy = (y >= h / 2) ? 2u : 0u;
        for (uint32_t x = 0; x < w; x++)
            row[x] = q[qy + ((x >= w / 2) ? 1u : 0u)];
    }
    kputs("[fb] gop pattern drawn\r\n");
}

void display_init(void) {
    if (g_boot_state == 0) {
        kputs("[fb] gop none\r\n");
        return;
    }
    if (g_boot_state == 2) {
        kputs("[fb] gop rejected: check mismatch\r\n");
        return;
    }
    int use = gop_geometry_sane() &&
              g_boot.format == BOOT_FB_FMT_BGRX &&
              !virtio_gpu_fb(0, 0, 0);          /* virtio-gpu, when present, wins */
    if (use) {
        uint64_t off   = g_boot.base & 0xFFFull;
        uint64_t bytes = (uint64_t)g_boot.stride_px * 4u * g_boot.height;
        uint64_t npg   = (off + bytes + 0xFFFull) >> 12;
        uint64_t pbase = g_boot.base & ~0xFFFull;
        for (uint64_t i = 0; i < npg; i++) {
            if (vmm_map(GOP_VBASE + (i << 12), pbase + (i << 12),
                        VMM_RW | VMM_NX) != 0) {
                use = 0;                        /* a partial map is not a display */
                break;
            }
        }
        if (use) {
            g_gop_kva  = (uint8_t *)(uintptr_t)(GOP_VBASE + off);
            g_gop_phys = g_boot.base;
        }
    }
    g_gop_used = use;

    { kline k; kline_init(&k);                    /* one write: DDR-1055 */
      kline_s(&k, "[fb] gop ");
      kline_d(&k, g_boot.width);  kline_s(&k, "x");
      kline_d(&k, g_boot.height);
      kline_s(&k, " stride="); kline_d(&k, g_boot.stride_px);
      kline_s(&k, " fmt=");    kline_d(&k, g_boot.format);
      kline_s(&k, " base=");   kline_x(&k, g_boot.base);
      kline_s(&k, " used=");   kline_d(&k, (uint64_t)use);
      kline_s(&k, "\r\n"); kline_emit(&k); }

    if (use && probe_enabled("gop"))
        gop_pattern();
}

uint8_t *display_fb(uint32_t *w, uint32_t *h, uint32_t *stride_bytes,
                    uint64_t *phys) {
    uint32_t vw, vh, vs;
    uint8_t *vfb = virtio_gpu_fb(&vw, &vh, &vs);
    if (vfb) {
        if (w) *w = vw;
        if (h) *h = vh;
        if (stride_bytes) *stride_bytes = vs;
        /* virtio-gpu's buffer is PMM memory inside the low identity map, so
         * its kernel pointer IS its physical address. That is a property of
         * THIS backend, stated here rather than assumed by sys_fb_map. */
        if (phys) *phys = (uint64_t)(uintptr_t)vfb;
        return vfb;
    }
    if (!g_gop_used)
        return 0;
    if (w) *w = g_boot.width;
    if (h) *h = g_boot.height;
    if (stride_bytes) *stride_bytes = g_boot.stride_px * 4u;
    if (phys) *phys = g_gop_phys;
    return g_gop_kva;
}

int display_present(void) {
    if (virtio_gpu_fb(0, 0, 0))
        return virtio_gpu_present();
    return g_gop_used ? 0 : -1;
}
