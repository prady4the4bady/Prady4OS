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
#define VBLK_MAX 8   /* DDR-771: 4->8 — matches BLK_MAX; MSI-X window relocated to
                      * 56-63 (clear of net@54/input@55) so >4 disks register */

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

/* DDR-BLK-1: per-request slot — multiple requests are in flight per disk.
 * Slot i's DMA header lives at reqbuf + i*32 (16B header + status byte). */
#define VBLK_NREQ 8
struct vreq {
    volatile int  used;
    volatile int  done;
    struct tcb   *waiter;
    /* DDR-776: stuck-request watchdog. t0 is the submit tick; warned makes the
     * diagnostic fire once per request. Read lock-free from the timer ISR. */
    volatile uint64_t t0;
    volatile uint64_t lba;
    volatile int      warned;
};

struct vblk {
    struct virtio_pci_dev dev;
    struct virtq          vq;
    struct blk_device     bd;
    uint64_t              reqbuf;
    struct vreq           req[VBLK_NREQ];
    int16_t               head2slot[256];  /* used-ring head -> slot (-1 free) */
    /* DDR-878 (item 47): a FIFO of submitters waiting for a free request slot.
     * This was a SINGLE `struct tcb *slot_waiter`, which a second waiter simply
     * overwrote — the first thread was then blocked with no record of it
     * anywhere and was never woken. The per-request `waiter` field is one slot
     * PER REQUEST and was always correct; this path had one slot PER DEVICE for
     * an unbounded number of waiters, which is the whole defect. */
    struct tcb           *slot_head;       /* oldest waiter, or 0 */
    struct tcb           *slot_tail;       /* newest waiter, or 0 */
    spinlock_t            compl_lock; /* DDR-714C3/BLK-1: guards the vq + all slot
                                       * state — submit and completion now overlap
                                       * across CPUs (short, non-sleeping sections) */
    volatile int          compl_ap;   /* proof: a completion ran on a non-BSP CPU */
};

/* DDR-878: one-shot witness that two submitters really do wait at once. */
static volatile int g_multiwait_seen;

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
        int s = (head >= 0 && head < 256) ? v->head2slot[head] : -1;
        if (head >= 0 && head < 256)
            v->head2slot[head] = -1;
        virtq_free_chain(&v->vq, head);
        if (s >= 0 && s < VBLK_NREQ) {          /* wake THIS request's submitter */
            v->req[s].done = 1;
            if (v->req[s].waiter) {
                struct tcb *w = v->req[s].waiter;
                v->req[s].waiter = 0;
                sched_unblock(w);
            }
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
#define VBLK_MSIX_BASE 56   /* DDR-771: 8-vector window 56-63, clear of timer(48),
                             * wake(49), net(54), input(55) */
static void vblk_msix0(void) { complete(&g_inst[0]); }
static void vblk_msix1(void) { complete(&g_inst[1]); }
static void vblk_msix2(void) { complete(&g_inst[2]); }
static void vblk_msix3(void) { complete(&g_inst[3]); }
static void vblk_msix4(void) { complete(&g_inst[4]); }
static void vblk_msix5(void) { complete(&g_inst[5]); }
static void vblk_msix6(void) { complete(&g_inst[6]); }
static void vblk_msix7(void) { complete(&g_inst[7]); }
static irq_handler_fn const vblk_msix_fn[VBLK_MAX] =
    { vblk_msix0, vblk_msix1, vblk_msix2, vblk_msix3,
      vblk_msix4, vblk_msix5, vblk_msix6, vblk_msix7 };

/* DDR-776: stuck-request watchdog (Section B#3 diagnosis). Driven from the timer
 * path, so it still runs when a submitter is blocked forever on a lost completion
 * — only the waiting thread is stuck, the timer keeps ticking. Prints once per
 * request so a failing CI run NAMES the stuck request instead of just timing out.
 *
 * Deliberately lock-free: taking compl_lock from the timer ISR would add a
 * deadlock surface to the very subsystem under investigation. All fields read are
 * volatile scalars and this is diagnostic output only — a torn read can at worst
 * print a stale LBA, and the watchdog never mutates driver state (S6). Work per
 * call is bounded at VBLK_MAX * VBLK_NREQ scalar checks (S2). */
#define VBLK_STUCK_TICKS 500u          /* 5 s @100 Hz */

void virtio_blk_watchdog(void) {
    for (unsigned i = 0; i < g_ninst; i++) {
        struct vblk *v = &g_inst[i];
        for (int s = 0; s < VBLK_NREQ; s++) {
            if (!v->req[s].used || v->req[s].done || v->req[s].warned)
                continue;
            if (g_ticks - v->req[s].t0 < VBLK_STUCK_TICKS)
                continue;
            v->req[s].warned = 1;      /* once per request — no log spam */
            kputs("[vblk] stuck dev="); kputdec(i);
            kputs(" slot=");           kputdec((uint64_t)s);
            kputs(" lba=");            kputdec(v->req[s].lba);
            kputs(" age=");            kputdec(g_ticks - v->req[s].t0);
            kputs(" ticks\r\n");
        }
    }
}

/* DDR-714C3 proof: 1 if any disk's completion handler ran on a non-BSP CPU. */
int virtio_blk_completed_on_ap(void) {
    for (unsigned i = 0; i < g_ninst; i++)
        if (g_inst[i].compl_ap)
            return 1;
    return 0;
}

/* DDR-BLK-1: multi-in-flight submit. All slot + vq state under compl_lock
 * (the locks-4 pattern from C3: waits publish BLOCKED under the lock via
 * sched_block_on, so completions on any CPU can't lose the wakeup). The old
 * one-in-flight busy sleep-mutex is gone — a caller blocks only on ITS OWN
 * request; others proceed concurrently, on any CPU. */
/* DDR-878: pop the oldest waiter and wake it. Called with compl_lock held, on
 * every path that frees a request slot. Waking ONE per freed slot (rather than
 * all) keeps the wake count equal to the resource count; the woken thread
 * re-checks in submit()'s loop and re-queues if another CPU took the slot
 * first, so a spurious wake is safe and a lost one is not possible. */
static void slot_wake_one(struct vblk *v) {
    struct tcb *w = v->slot_head;
    if (!w)
        return;
    v->slot_head = w->blk_wait_next;
    if (!v->slot_head)
        v->slot_tail = 0;
    w->blk_wait_next = 0;
    sched_unblock(w);
}

static int submit(struct vblk *v, uint64_t lba, uint64_t data_phys,
                  uint32_t count, int to_device) {
    uint64_t fl = spin_lock_irqsave(&v->compl_lock);

    /* Claim a request slot; sleep (never spin) when all are in flight. */
    int s;
    for (;;) {
        for (s = 0; s < VBLK_NREQ; s++)
            if (!v->req[s].used)
                break;
        if (s < VBLK_NREQ)
            break;
        /* Enqueue at the tail, then block. Both under compl_lock, which is the
         * same lock the release path takes — that is what makes the
         * check-then-block safe (the locks-4 pattern). */
        current_thread->blk_wait_next = 0;
        if (v->slot_tail) {
            /* This is the old bug's precondition, made observable: a SECOND
             * submitter queueing while a first is already waiting is exactly
             * the case the single `slot_waiter` pointer overwrote. Printed once
             * so a gate can assert it actually happens — without it, "the fix
             * works" would rest on the queue never being exercised at all. */
            if (!g_multiwait_seen) {
                g_multiwait_seen = 1;
                kputs("[vblk] slot wait list depth>=2\r\n");
            }
            v->slot_tail->blk_wait_next = current_thread;
        } else {
            v->slot_head = current_thread;
        }
        v->slot_tail = current_thread;
        sched_block_on(&v->compl_lock);        /* woken when a slot frees */
    }
    v->req[s].used = 1;
    v->req[s].done = 0;
    v->req[s].waiter = current_thread;
    v->req[s].t0 = g_ticks;              /* DDR-776: watchdog arm point */
    v->req[s].lba = lba;
    v->req[s].warned = 0;

    uint64_t hdr = v->reqbuf + (uint64_t)s * 32;   /* 16B header + status byte */
    struct virtio_blk_req *h = (struct virtio_blk_req *)(uintptr_t)hdr;
    volatile uint8_t *status = (volatile uint8_t *)(uintptr_t)(hdr + 16);
    h->type = to_device ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    h->reserved = 0;
    h->sector = lba;
    *status = 0xFF;

    struct virtq_buf bufs[3] = {
        { hdr,       sizeof(struct virtio_blk_req), 0 },
        { data_phys, SECTOR * count,                to_device ? 0 : 1 },
        { hdr + 16,  1,                             1 },
    };
    int head = virtq_add(&v->vq, bufs, 3);
    if (head < 0 || head >= 256) {
        v->req[s].used = 0;
        v->req[s].waiter = 0;
        /* This path frees a slot too. The original woke nobody here, so a
         * descriptor-exhaustion failure could strand every waiter even with the
         * single-waiter bug fixed. Same release, same wake. */
        slot_wake_one(v);
        spin_unlock_irqrestore(&v->compl_lock, fl);
        return -1;
    }
    v->head2slot[head] = (int16_t)s;
    virtq_publish(&v->vq, head);
    virtio_pci_notify(&v->dev, &v->vq, 0);

    while (!v->req[s].done)
        sched_block_on(&v->compl_lock);        /* BLOCKED published under the lock */

    int ok = (*status == 0);
    v->req[s].used = 0;                        /* release the slot ... */
    v->req[s].waiter = 0;
    slot_wake_one(v);                          /* ... and wake ONE starved submitter */
    spin_unlock_irqrestore(&v->compl_lock, fl);
    return ok ? 0 : -1;
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
    v->slot_head = 0;
    v->slot_tail = 0;
    for (int i = 0; i < VBLK_NREQ; i++) {      /* DDR-BLK-1: request slots */
        v->req[i].used = 0;
        v->req[i].done = 0;
        v->req[i].waiter = 0;
    }
    for (int i = 0; i < 256; i++)
        v->head2slot[i] = -1;
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
