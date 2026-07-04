/* kernel/drivers/blk/virtio_blk.c — virtio-blk over the virtio transport.
 *
 * Multi-instance: each disk gets its own transport handle, virtqueue, and
 * request buffer. A request is a 3-descriptor chain (header RO | data | status
 * WO). Completion is interrupt-driven; the shared INTx handler checks every
 * instance's ISR (INTx is level-triggered and may be shared) and wakes the
 * blocked caller. One request in flight per disk.
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

#define VIRTIO_BLK_T_IN       0
#define VIRTIO_BLK_T_OUT      1
#define VIRTIO_BLK_F_SIZE_MAX (1u << 1)
#define VIRTIO_BLK_F_SEG_MAX  (1u << 2)
#define SECTOR 512u
#define VBLK_MAX 4

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

struct vblk {
    struct virtio_pci_dev dev;
    struct virtq          vq;
    struct blk_device     bd;
    uint64_t              reqbuf;
    volatile int          done;
    volatile int          busy;       /* one request in flight per disk */
    struct tcb           *waiter;
};

static struct vblk g_inst[VBLK_MAX];
static unsigned    g_ninst;

static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }

static void reap(struct vblk *v) {
    uint8_t isr = virtio_pci_isr_ack(&v->dev);   /* read-to-clear, deasserts INTx */
    if (!(isr & 1))
        return;
    uint32_t len;
    int head;
    while ((head = virtq_pop_used(&v->vq, &len)) >= 0) {
        virtq_free_chain(&v->vq, head);
        v->done = 1;
        if (v->waiter) {
            struct tcb *w = v->waiter;
            v->waiter = 0;
            sched_unblock(w);
        }
    }
}

/* Shared INTx handler: a level-triggered line may be shared, so poll all. */
static void virtio_blk_irq(void) {
    for (unsigned i = 0; i < g_ninst; i++)
        reap(&g_inst[i]);
}

static int submit(struct vblk *v, uint64_t lba, uint64_t data_phys,
                  uint32_t count, int to_device) {
    struct virtio_blk_req *h = (struct virtio_blk_req *)(uintptr_t)v->reqbuf;
    volatile uint8_t *status = (volatile uint8_t *)(uintptr_t)(v->reqbuf + 16);
    h->type = to_device ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    h->reserved = 0;
    h->sector = lba;
    *status = 0xFF;

    struct virtq_buf bufs[3] = {
        { v->reqbuf,      sizeof(struct virtio_blk_req), 0 },
        { data_phys,      SECTOR * count,                to_device ? 0 : 1 },
        { v->reqbuf + 16, 1,                             1 },
    };

    cli();
    /* DDR-SMP-3c-locks-2: sleep-mutex, held across sched_block() below — so the
     * acquire is an atomic exchange (race-free across CPUs), NOT a spinlock (a
     * spinlock held across a block would deadlock spinners). The loser reads 1
     * and yields, exactly the prior one-CPU behavior. */
    while (__atomic_exchange_n(&v->busy, 1, __ATOMIC_ACQUIRE)) {
        sti();
        yield();
        cli();
    }
    v->done = 0;
    v->waiter = current_thread;
    int head = virtq_add(&v->vq, bufs, 3);
    if (head < 0) {
        __atomic_store_n(&v->busy, 0, __ATOMIC_RELEASE);
        sti();
        return -1;
    }
    virtq_publish(&v->vq, head);
    virtio_pci_notify(&v->dev, &v->vq, 0);
    while (!v->done)
        sched_block();
    __atomic_store_n(&v->busy, 0, __ATOMIC_RELEASE);
    sti();
    return (*status == 0) ? 0 : -1;
}

static int vblk_read(struct blk_device *bd, uint64_t lba, void *buf, uint32_t count) {
    return submit((struct vblk *)bd->drv, lba, (uint64_t)(uintptr_t)buf, count, 0);
}
static int vblk_write(struct blk_device *bd, uint64_t lba, const void *buf, uint32_t count) {
    return submit((struct vblk *)bd->drv, lba, (uint64_t)(uintptr_t)buf, count, 1);
}

void virtio_blk_init(uint8_t bus, uint8_t dev, uint8_t func) {
    if (g_ninst >= VBLK_MAX)
        return;
    struct vblk *v = &g_inst[g_ninst];

    if (virtio_pci_attach(&v->dev, bus, dev, func) != 0) {
        kputs("virtio-blk: attach failed\r\n");
        return;
    }
    uint64_t want = VIRTIO_F_VERSION_1 | VIRTIO_BLK_F_SIZE_MAX | VIRTIO_BLK_F_SEG_MAX;
    if (!(virtio_pci_negotiate(&v->dev, want) & VIRTIO_F_VERSION_1)) {
        kputs("virtio-blk: modern negotiation failed\r\n");
        return;
    }
    if (virtio_pci_setup_queue(&v->dev, &v->vq, 0) != 0) {
        kputs("virtio-blk: queue setup failed\r\n");
        return;
    }
    v->reqbuf = pmm_alloc_page();
    if (!v->reqbuf) {
        kputs("virtio-blk: no request buffer\r\n");
        return;
    }
    uint64_t capacity = *(volatile uint64_t *)(uintptr_t)v->dev.devcfg;

    irq_register(v->dev.irq, virtio_blk_irq);
    pic_unmask(v->dev.irq);
    virtio_pci_driver_ok(&v->dev);

    v->bd.name = "virtio-blk";
    v->bd.capacity_sectors = capacity;
    v->bd.read = vblk_read;
    v->bd.write = vblk_write;
    v->bd.drv = v;
    blk_register(&v->bd);
    g_ninst++;

    kputs("virtio-blk: blk");
    kputdec(g_ninst - 1);
    kputs(" ready, ");
    kputdec(capacity);
    kputs(" sectors, IRQ ");
    kputdec(v->dev.irq);
    kputs("\r\n");
}
