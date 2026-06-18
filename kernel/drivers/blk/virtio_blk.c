/* kernel/drivers/blk/virtio_blk.c — virtio-blk over the virtio transport.
 *
 * One in-flight request at a time (single queue). A request is a 3-descriptor
 * chain: header (RO) | data (device-write for reads / RO for writes) | status
 * (device-write). Completion is interrupt-driven: the INTx handler reaps the
 * used ring and wakes the blocked caller — no busy polling.
 */
#include "virtio_blk.h"
#include "blk.h"
#include "virtio.h"
#include "virtio_pci.h"
#include "console.h"
#include "pmm.h"
#include "sched.h"
#include "irq.h"

extern void irq_register(unsigned irq, void (*fn)(void));   /* kernel/idt.c */

#define VIRTIO_BLK_T_IN       0          /* read  */
#define VIRTIO_BLK_T_OUT      1          /* write */
#define VIRTIO_BLK_F_SIZE_MAX (1u << 1)
#define VIRTIO_BLK_F_SEG_MAX  (1u << 2)
#define SECTOR 512u

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

static struct virtio_pci_dev g_dev;
static struct virtq          g_vq;
static struct blk_device     g_bd;
static uint64_t              g_reqbuf;    /* PMM page: header @0, status @16 */
static volatile int          g_done;
static struct tcb           *g_waiter;

static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }

static void virtio_blk_irq(void) {
    uint8_t isr = virtio_pci_isr_ack(&g_dev);   /* read-to-clear, deasserts INTx */
    if (!(isr & 1))
        return;                                  /* not a used-ring interrupt */
    uint32_t len;
    int head;
    while ((head = virtq_pop_used(&g_vq, &len)) >= 0) {
        virtq_free_chain(&g_vq, head);
        g_done = 1;
        if (g_waiter) {
            struct tcb *w = g_waiter;
            g_waiter = 0;
            sched_unblock(w);
        }
    }
}

/* Submit a header/data/status chain and block until the IRQ signals completion.
 * `to_device` = 1 for writes (data is read-only to the device). Returns 0 if the
 * device reported status 0. */
static int submit(uint64_t lba, uint64_t data_phys, uint32_t count, int to_device) {
    struct virtio_blk_req *h = (struct virtio_blk_req *)(uintptr_t)g_reqbuf;
    volatile uint8_t *status = (volatile uint8_t *)(uintptr_t)(g_reqbuf + 16);
    h->type = to_device ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    h->reserved = 0;
    h->sector = lba;
    *status = 0xFF;

    struct virtq_buf bufs[3] = {
        { g_reqbuf,       sizeof(struct virtio_blk_req), 0 },
        { data_phys,      SECTOR * count,                to_device ? 0 : 1 },
        { g_reqbuf + 16,  1,                             1 },
    };

    cli();
    g_done = 0;
    g_waiter = current_thread;
    int head = virtq_add(&g_vq, bufs, 3);
    if (head < 0) {
        sti();
        return -1;
    }
    virtq_publish(&g_vq, head);
    virtio_pci_notify(&g_dev, &g_vq, 0);
    while (!g_done)
        sched_block();
    sti();
    return (*status == 0) ? 0 : -1;
}

static int vblk_read(struct blk_device *bd, uint64_t lba, void *buf, uint32_t count) {
    (void)bd;
    return submit(lba, (uint64_t)(uintptr_t)buf, count, 0);
}

static int vblk_write(struct blk_device *bd, uint64_t lba, const void *buf, uint32_t count) {
    (void)bd;
    return submit(lba, (uint64_t)(uintptr_t)buf, count, 1);
}

void virtio_blk_init(uint8_t bus, uint8_t dev, uint8_t func) {
    if (virtio_pci_attach(&g_dev, bus, dev, func) != 0) {
        kputs("virtio-blk: attach failed\r\n");
        return;
    }
    uint64_t want = VIRTIO_F_VERSION_1 | VIRTIO_BLK_F_SIZE_MAX | VIRTIO_BLK_F_SEG_MAX;
    uint64_t got = virtio_pci_negotiate(&g_dev, want);
    if (!(got & VIRTIO_F_VERSION_1)) {
        kputs("virtio-blk: modern (VERSION_1) negotiation failed\r\n");
        return;
    }
    if (virtio_pci_setup_queue(&g_dev, &g_vq, 0) != 0) {
        kputs("virtio-blk: queue setup failed\r\n");
        return;
    }
    g_reqbuf = pmm_alloc_page();
    if (!g_reqbuf) {
        kputs("virtio-blk: no request buffer\r\n");
        return;
    }

    uint64_t capacity = *(volatile uint64_t *)(uintptr_t)g_dev.devcfg;  /* sectors */

    irq_register(g_dev.irq, virtio_blk_irq);
    pic_unmask(g_dev.irq);
    virtio_pci_driver_ok(&g_dev);

    g_bd.name = "virtio-blk";
    g_bd.capacity_sectors = capacity;
    g_bd.read = vblk_read;
    g_bd.write = vblk_write;
    g_bd.drv = &g_dev;
    blk_register(&g_bd);

    kputs("virtio-blk: ready, capacity ");
    kputdec(capacity);
    kputs(" sectors, INTx IRQ ");
    kputdec(g_dev.irq);
    kputs("\r\n");
}
