/* kernel/drivers/input/virtio_input.c — virtio-input absolute pointer (DDR-705).
 *
 * Reuses the virtio-pci/virtq transport. Sets up the eventq (queue 0) with a pool
 * of writable 8-byte buffers; the device writes one virtio_input_event per buffer
 * and returns it on the used ring. The IRQ handler folds events into a current
 * {abs_x, abs_y, buttons} state and re-arms the buffer (same pattern as the
 * virtio-net RX path). SYS_MOUSE_POLL reads the state, mapped to screen pixels.
 */
#include "virtio_input.h"
#include "virtio.h"
#include "virtio_pci.h"
#include "pmm.h"
#include "console.h"
#include "irq.h"             /* irq_register / pic_unmask */
#include "virtio_gpu.h"      /* screen geometry for abs->pixel mapping */

extern void irq_register(unsigned irq, void (*fn)(void));   /* kernel/idt.c */

/* Linux input event codes (subset). */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define REL_X  0x00
#define REL_Y  0x01
#define ABS_X  0x00
#define ABS_Y  0x01
#define BTN_LEFT 0x110
#define VI_ABS_MAX 32768          /* virtio-tablet axis range is [0, 32767] */

struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((packed));
_Static_assert(sizeof(struct virtio_input_event) == 8, "virtio_input_event = 8 bytes");

#define VI_NBUF 32

static struct virtio_pci_dev g_dev;
static struct virtq g_vq;
static uint64_t g_pool;           /* PMM page: VI_NBUF * 8-byte event buffers */
static int g_up;

static volatile int      g_abs_x, g_abs_y;   /* raw axis value [0, 32767] */
static volatile uint32_t g_buttons;          /* bit0 = left, bit1 = right, bit2 = middle */

static void fold_event(const struct virtio_input_event *e) {
    switch (e->type) {
    case EV_ABS:
        if (e->code == ABS_X) g_abs_x = (int)e->value;
        else if (e->code == ABS_Y) g_abs_y = (int)e->value;
        break;
    case EV_REL:                          /* relative mice: accumulate, clamp */
        if (e->code == REL_X) g_abs_x = (int)((uint32_t)g_abs_x + e->value);
        else if (e->code == REL_Y) g_abs_y = (int)((uint32_t)g_abs_y + e->value);
        if (g_abs_x < 0) g_abs_x = 0; if (g_abs_x >= VI_ABS_MAX) g_abs_x = VI_ABS_MAX - 1;
        if (g_abs_y < 0) g_abs_y = 0; if (g_abs_y >= VI_ABS_MAX) g_abs_y = VI_ABS_MAX - 1;
        break;
    case EV_KEY:
        if (e->code >= BTN_LEFT && e->code <= BTN_LEFT + 2) {
            uint32_t bit = 1u << (e->code - BTN_LEFT);
            if (e->value) g_buttons |= bit; else g_buttons &= ~bit;
        }
        break;
    default: break;                       /* EV_SYN etc.: state already coherent */
    }
}

static void input_irq(void) {
    uint8_t isr = virtio_pci_isr_ack(&g_dev);
    if (!(isr & 1))
        return;
    uint32_t len;
    int head;
    int budget = 256;                     /* bound work per IRQ */
    while (budget-- > 0 && (head = virtq_pop_used(&g_vq, &len)) >= 0) {
        uint64_t buf = g_vq.desc[head].addr;
        virtq_free_chain(&g_vq, head);
        if (len >= sizeof(struct virtio_input_event))
            fold_event((const struct virtio_input_event *)(uintptr_t)buf);
        struct virtq_buf rb = { buf, sizeof(struct virtio_input_event), 1 };  /* re-arm */
        int h = virtq_add(&g_vq, &rb, 1);
        if (h >= 0)
            virtq_publish(&g_vq, h);
    }
    virtio_pci_notify(&g_dev, &g_vq, 0);
}

void virtio_input_init(uint8_t bus, uint8_t dev, uint8_t func) {
    if (g_up)
        return;                           /* one pointer is enough (ignore extras) */
    if (virtio_pci_attach(&g_dev, bus, dev, func) != 0) {
        kputs("virtio-input: attach failed\r\n");
        return;
    }
    if (!(virtio_pci_negotiate(&g_dev, VIRTIO_F_VERSION_1) & VIRTIO_F_VERSION_1)) {
        kputs("virtio-input: modern negotiation failed\r\n");
        return;
    }
    if (virtio_pci_setup_queue(&g_dev, &g_vq, 0) != 0) {     /* eventq */
        kputs("virtio-input: eventq setup failed\r\n");
        return;
    }
    g_pool = pmm_alloc_page();
    if (!g_pool) { kputs("virtio-input: no buffer pool\r\n"); return; }

    for (int i = 0; i < VI_NBUF; i++) {   /* arm the eventq with writable buffers */
        struct virtq_buf eb = { g_pool + (uint64_t)i * sizeof(struct virtio_input_event),
                                sizeof(struct virtio_input_event), 1 };
        int h = virtq_add(&g_vq, &eb, 1);
        if (h >= 0)
            virtq_publish(&g_vq, h);
    }
    g_abs_x = VI_ABS_MAX / 2;             /* start centred */
    g_abs_y = VI_ABS_MAX / 2;

    irq_register(g_dev.irq, input_irq);
    pic_unmask(g_dev.irq);
    virtio_pci_driver_ok(&g_dev);
    virtio_pci_notify(&g_dev, &g_vq, 0);
    g_up = 1;
    kputs("[input] virtio pointer up (eventq armed)\r\n");
}

int virtio_input_state(int *x, int *y, uint32_t *buttons) {
    if (!g_up)
        return -1;
    uint32_t w = 0, h = 0;
    if (!virtio_gpu_fb(&w, &h, 0) || w == 0 || h == 0) { w = 1024; h = 768; }
    if (x) *x = (int)((uint64_t)g_abs_x * w / VI_ABS_MAX);
    if (y) *y = (int)((uint64_t)g_abs_y * h / VI_ABS_MAX);
    if (buttons) *buttons = g_buttons;
    return 0;
}
