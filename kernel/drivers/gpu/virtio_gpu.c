/* kernel/drivers/gpu/virtio_gpu.c — VirtIO-GPU 2D framebuffer bring-up (ADR-028).
 *
 * Slice 0 of Layer 7: the first display-output path. Reuses the shared
 * virtio_pci/virtq transport (like virtio-blk/net). Bring-up runs once in kmain
 * and POLLS the control queue (no IRQ dependency). Proves the full 2D path —
 * GET_DISPLAY_INFO -> CREATE_2D -> ATTACH_BACKING -> SET_SCANOUT -> TRANSFER ->
 * FLUSH — and presents a linear BGRA framebuffer. Headless QEMU still ACKs every
 * command, so the path is verifiable without a display (smoke-gpu).
 */
#include "virtio_gpu.h"
#include "virtio.h"
#include "virtio_pci.h"
#include "pmm.h"
#include "console.h"
#include "string.h"

/* ---- 2D control protocol (virtio 1.1 §5.7) -------------------------------- */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO       0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D     0x0101
#define VIRTIO_GPU_CMD_SET_SCANOUT            0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH         0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D    0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_RESP_OK_NODATA             0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO       0x1101
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM      1

struct gpu_ctrl_hdr {
    uint32_t type, flags;
    uint64_t fence_id;
    uint32_t ctx_id, padding;
};
struct gpu_rect { uint32_t x, y, width, height; };

struct gpu_disp_one { struct gpu_rect r; uint32_t enabled, flags; };
struct gpu_resp_display_info { struct gpu_ctrl_hdr hdr; struct gpu_disp_one pmodes[16]; };
struct gpu_create_2d { struct gpu_ctrl_hdr hdr; uint32_t resource_id, format, width, height; };
struct gpu_attach_backing { struct gpu_ctrl_hdr hdr; uint32_t resource_id, nr_entries;
                            uint64_t addr; uint32_t length, padding; };
struct gpu_set_scanout { struct gpu_ctrl_hdr hdr; struct gpu_rect r; uint32_t scanout_id, resource_id; };
struct gpu_transfer_2d { struct gpu_ctrl_hdr hdr; struct gpu_rect r; uint64_t offset;
                         uint32_t resource_id, padding; };
struct gpu_resource_flush { struct gpu_ctrl_hdr hdr; struct gpu_rect r; uint32_t resource_id, padding; };

#define GPU_RESOURCE_ID 1
#define GPU_REQ_OFF     0u      /* request region within the scratch page */
#define GPU_RESP_OFF    1024u   /* response region */

struct vgpu {
    struct virtio_pci_dev dev;
    struct virtq vq;            /* control queue (0) */
    uint64_t scratch;           /* one PMM page: request @+0, response @+1024 */
    uint8_t *fb;
    uint32_t w, h, stride;
};
static struct vgpu g_gpu;
static int g_up;

/* Submit a request (req_len bytes at scratch) + a response buffer (resp_len at
 * scratch+1024), poll for completion, return the device-written response type. */
static uint32_t gpu_cmd(struct vgpu *g, uint32_t req_len, uint32_t resp_len) {
    volatile struct gpu_ctrl_hdr *resp =
        (volatile struct gpu_ctrl_hdr *)(uintptr_t)(g->scratch + GPU_RESP_OFF);
    resp->type = 0;
    struct virtq_buf bufs[2] = {
        { g->scratch + GPU_REQ_OFF,  req_len,  0 },   /* device reads the request  */
        { g->scratch + GPU_RESP_OFF, resp_len, 1 },   /* device writes the response */
    };
    int head = virtq_add(&g->vq, bufs, 2);
    if (head < 0)
        return 0;
    virtq_publish(&g->vq, head);
    virtio_pci_notify(&g->dev, &g->vq, 0);
    /* Wait on the used ring with HLT (interrupts are enabled this early — sti at
     * kmain). HLT releases the core so QEMU's single-threaded TCG can process the
     * kick; the PIT (100 Hz) wakes us to re-check. A busy spin instead starves the
     * device backend and never converges with several virtio devices attached. */
    for (uint64_t tick = 0; tick < 2000; tick++) {   /* ~20 s cap @ 10 ms/PIT */
        uint32_t len;
        int h = virtq_pop_used(&g->vq, &len);
        if (h >= 0) {
            virtq_free_chain(&g->vq, h);
            return resp->type;
        }
        (void)virtio_pci_isr_ack(&g->dev);            /* deassert any shared INTx */
        __asm__ volatile("hlt");
    }
    kputs("virtio-gpu: cmd poll timeout\r\n");
    return 0;                                          /* timeout */
}

static void hdr_set(uint32_t type) {
    struct gpu_ctrl_hdr *h = (struct gpu_ctrl_hdr *)(uintptr_t)(g_gpu.scratch + GPU_REQ_OFF);
    memset(h, 0, 64);
    h->type = type;
}

void virtio_gpu_init(uint8_t bus, uint8_t dev, uint8_t func) {
    struct vgpu *g = &g_gpu;
    if (virtio_pci_attach(&g->dev, bus, dev, func) != 0) {
        kputs("virtio-gpu: attach failed\r\n");
        return;
    }
    if (!(virtio_pci_negotiate(&g->dev, VIRTIO_F_VERSION_1) & VIRTIO_F_VERSION_1)) {
        kputs("virtio-gpu: modern negotiation failed\r\n");
        return;
    }
    if (virtio_pci_setup_queue(&g->dev, &g->vq, 0) != 0) {   /* control queue */
        kputs("virtio-gpu: control queue setup failed\r\n");
        return;
    }
    /* Bring-up polls the used ring; suppress this queue's used interrupt so the
     * GPU never storms its (shared) INTx line while we wait. */
    g->vq.avail->flags = 1;                                  /* VIRTQ_AVAIL_F_NO_INTERRUPT */
    g->scratch = pmm_alloc_page();
    if (!g->scratch) { kputs("virtio-gpu: no scratch page\r\n"); return; }
    virtio_pci_driver_ok(&g->dev);

    /* 1) Query the display. Use scanout 0's mode if sane, else 1024x768. */
    hdr_set(VIRTIO_GPU_CMD_GET_DISPLAY_INFO);
    g->w = 1024; g->h = 768;
    if (gpu_cmd(g, sizeof(struct gpu_ctrl_hdr),
                sizeof(struct gpu_resp_display_info)) == VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        struct gpu_resp_display_info *di =
            (struct gpu_resp_display_info *)(uintptr_t)(g->scratch + GPU_RESP_OFF);
        uint32_t w = di->pmodes[0].r.width, h = di->pmodes[0].r.height;
        if (di->pmodes[0].enabled && w >= 320 && w <= 1024 && h >= 240 && h <= 768) {
            g->w = w; g->h = h;
        }
    }
    g->stride = g->w * 4;

    /* Framebuffer: w*h*4 from the PMM pool (phys==virt, identity-mapped). */
    uint64_t fb_bytes = (uint64_t)g->stride * g->h;
    unsigned order = 0;
    while (((uint64_t)4096u << order) < fb_bytes && order < 10) order++;
    uint64_t fb = pmm_alloc_pages(order);
    if (!fb) { kputs("virtio-gpu: no framebuffer\r\n"); return; }
    g->fb = (uint8_t *)(uintptr_t)fb;

    /* 2) Create a 2D BGRA resource. */
    hdr_set(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    struct gpu_create_2d *c = (struct gpu_create_2d *)(uintptr_t)(g->scratch + GPU_REQ_OFF);
    c->resource_id = GPU_RESOURCE_ID;
    c->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    c->width = g->w; c->height = g->h;
    if (gpu_cmd(g, sizeof *c, sizeof(struct gpu_ctrl_hdr)) != VIRTIO_GPU_RESP_OK_NODATA) {
        kputs("virtio-gpu: create_2d failed\r\n"); return;
    }

    /* 3) Attach the framebuffer as the resource's backing store. */
    hdr_set(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    struct gpu_attach_backing *a = (struct gpu_attach_backing *)(uintptr_t)(g->scratch + GPU_REQ_OFF);
    a->resource_id = GPU_RESOURCE_ID;
    a->nr_entries = 1;
    a->addr = fb;
    a->length = (uint32_t)fb_bytes;
    if (gpu_cmd(g, sizeof *a, sizeof(struct gpu_ctrl_hdr)) != VIRTIO_GPU_RESP_OK_NODATA) {
        kputs("virtio-gpu: attach_backing failed\r\n"); return;
    }

    /* 4) Bind the resource to scanout 0 (mode-set). */
    hdr_set(VIRTIO_GPU_CMD_SET_SCANOUT);
    struct gpu_set_scanout *s = (struct gpu_set_scanout *)(uintptr_t)(g->scratch + GPU_REQ_OFF);
    s->r.x = 0; s->r.y = 0; s->r.width = g->w; s->r.height = g->h;
    s->scanout_id = 0; s->resource_id = GPU_RESOURCE_ID;
    if (gpu_cmd(g, sizeof *s, sizeof(struct gpu_ctrl_hdr)) != VIRTIO_GPU_RESP_OK_NODATA) {
        kputs("virtio-gpu: set_scanout failed\r\n"); return;
    }

    /* 5) Draw a deterministic test pattern (diagonal gradient) and present it. */
    for (uint32_t y = 0; y < g->h; y++) {
        uint8_t *row = g->fb + (uint64_t)y * g->stride;
        for (uint32_t x = 0; x < g->w; x++) {
            row[x * 4 + 0] = (uint8_t)(x);            /* B */
            row[x * 4 + 1] = (uint8_t)(y);            /* G */
            row[x * 4 + 2] = (uint8_t)((x + y) >> 1); /* R */
            row[x * 4 + 3] = 0xFF;                    /* A */
        }
    }
    hdr_set(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    struct gpu_transfer_2d *t = (struct gpu_transfer_2d *)(uintptr_t)(g->scratch + GPU_REQ_OFF);
    t->r.x = 0; t->r.y = 0; t->r.width = g->w; t->r.height = g->h;
    t->offset = 0; t->resource_id = GPU_RESOURCE_ID;
    if (gpu_cmd(g, sizeof *t, sizeof(struct gpu_ctrl_hdr)) != VIRTIO_GPU_RESP_OK_NODATA) {
        kputs("virtio-gpu: transfer_2d failed\r\n"); return;
    }
    hdr_set(VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    struct gpu_resource_flush *f = (struct gpu_resource_flush *)(uintptr_t)(g->scratch + GPU_REQ_OFF);
    f->r.x = 0; f->r.y = 0; f->r.width = g->w; f->r.height = g->h;
    f->resource_id = GPU_RESOURCE_ID;
    if (gpu_cmd(g, sizeof *f, sizeof(struct gpu_ctrl_hdr)) != VIRTIO_GPU_RESP_OK_NODATA) {
        kputs("virtio-gpu: resource_flush failed\r\n"); return;
    }

    g_up = 1;
    kputs("PRADYOS_GPU_FB_OK ");
    kputdec(g->w); kputs("x"); kputdec(g->h);
    kputs(" BGRA scanout0\r\n");
}

uint8_t *virtio_gpu_fb(uint32_t *w, uint32_t *h, uint32_t *stride) {
    if (!g_up) return 0;
    if (w) *w = g_gpu.w;
    if (h) *h = g_gpu.h;
    if (stride) *stride = g_gpu.stride;
    return g_gpu.fb;
}
