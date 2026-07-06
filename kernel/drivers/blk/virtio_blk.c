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
#include "lapic.h"   /* DDR-714C1: lapic_id() — MSI-X destination (the BSP) */

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
    spinlock_t            compl_lock; /* DDR-714C3: guards done/waiter/vq-complete —
                                       * the IRQ can run on ANOTHER CPU now */
    volatile int          compl_ap;   /* proof: a completion ran on a non-BSP CPU */
};

static struct vblk g_inst[VBLK_MAX];
static unsigned    g_ninst;

static void complete(struct vblk *v) {
    /* DDR-714C3: runs on whatever CPU the vector targets. The lock closes the
     * lost-wakeup race against submit()'s check-then-block (the locks-4
     * pattern: the requester publishes BLOCKED under this same lock). */
    uint64_t fl = spin_lock_irqsave(&v->compl_lock);
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
    struct percpu *pc = this_cpu();
    if (pc && !pc->is_bsp)
        v->compl_ap = 1;              /* C3 proof: completion off the BSP */
    spin_unlock_irqrestore(&v->compl_lock, fl);
}

static void reap(struct vblk *v) {
    uint8_t isr = virtio_pci_isr_ack(&v->dev);   /* read-to-clear, deasserts INTx */
    if (!(isr & 1))
        return;
    complete(v);
}

/* Shared INTx handler: a level-triggered line may be shared, so poll all. */
static void virtio_blk_irq(void) {
    for (unsigned i = 0; i < g_ninst; i++)
        reap(&g_inst[i]);
}

/* DDR-714C1: per-device MSI-X handlers — unshared vector, no ISR-ack read
 * (that register is the INTx deassert; MSI-X does not use it). */
#define VBLK_MSIX_BASE 50
static void vblk_msix0(void) { complete(&g_inst[0]); }
static void vblk_msix1(void) { complete(&g_inst[1]); }
static void vblk_msix2(void) { complete(&g_inst[2]); }
static void vblk_msix3(void) { complete(&g_inst[3]); }
static irq_handler_fn const vblk_msix_fn[VBLK_MAX] =
    { vblk_msix0, vblk_msix1, vblk_msix2, vblk_msix3 };

/* DDR-714C3 proof: 1 if any disk's completion handler ran on a non-BSP CPU. */
int virtio_blk_completed_on_ap(void) {
    for (unsigned i = 0; i < g_ninst; i++)
        if (g_inst[i].compl_ap)
            return 1;
    return 0;
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

    /* DDR-SMP-3c-locks-2: per-disk sleep-mutex (atomic acquire, yield-wait) —
     * one request in flight; NOT a spinlock (held across the sleep below). */
    while (__atomic_exchange_n(&v->busy, 1, __ATOMIC_ACQUIRE))
        yield();

    /* DDR-714C3: the completion IRQ may run on another CPU, so the old cli
     * around done/waiter no longer closes the lost-wakeup race. The locks-4
     * pattern does: done/waiter (and the publish+notify, kept short) live under
     * compl_lock, and the wait publishes BLOCKED under that same lock via
     * sched_block_on — a completion serialized after the release always sees
     * the waiter BLOCKED, so its wake CAS cannot miss. */
    uint64_t fl = spin_lock_irqsave(&v->compl_lock);
    v->done = 0;
    v->waiter = current_thread;
    int head = virtq_add(&v->vq, bufs, 3);
    if (head < 0) {
        v->waiter = 0;
        spin_unlock_irqrestore(&v->compl_lock, fl);
        __atomic_store_n(&v->busy, 0, __ATOMIC_RELEASE);
        return -1;
    }
    virtq_publish(&v->vq, head);
    virtio_pci_notify(&v->dev, &v->vq, 0);
    while (!v->done)
        sched_block_on(&v->compl_lock);
    v->waiter = 0;
    spin_unlock_irqrestore(&v->compl_lock, fl);
    __atomic_store_n(&v->busy, 0, __ATOMIC_RELEASE);
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

    /* DDR-714C1: prefer a per-device MSI-X vector (50+unit) — unshared, no
     * 8259; INTx fallback. DDR-714C3: distribute the vectors round-robin over
     * the APs (unit i -> roster 1+(i%(n-1))) so completions run off the BSP;
     * single-CPU boots keep the BSP. The AP LAPICs are sw-enabled long before
     * the first disk I/O (the FS phase runs under the scheduler). */
    unsigned unit = g_ninst;
    uint8_t vec = (uint8_t)(VBLK_MSIX_BASE + unit);
    v->compl_lock = (spinlock_t)SPINLOCK_INIT;
    v->compl_ap = 0;
    unsigned ncpu = lapic_cpu_count();
    uint32_t dest = (ncpu > 1) ? lapic_apic_id_at(1 + (unit % (ncpu - 1)))
                               : lapic_id();
    int msix = (virtio_pci_msix_setup(&v->dev, vec, dest, 1) == 0);
    if (msix) {
        msix_register(vec, vblk_msix_fn[unit]);
    } else {
        irq_register(v->dev.irq, virtio_blk_irq);
        pic_unmask(v->dev.irq);
    }
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
    if (msix) {
        kputs(" sectors, msix vec=");
        kputdec(vec);
    } else {
        kputs(" sectors, IRQ ");
        kputdec(v->dev.irq);
    }
    kputs("\r\n");
}
