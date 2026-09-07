/* kernel/drivers/nvme/nvme.c — NVMe driver (DDR-765 bring-up + DDR-766 block I/O).
 *
 * Detects one NVMe controller (PCIe class 0x01 / subclass 0x08), maps BAR0
 * uncached, enables the controller, and stands up an admin queue to run Identify
 * Controller + Identify Namespace (DDR-765). DDR-766 then creates one I/O queue
 * pair and registers the namespace as a generic block device ("nvme0"), with a
 * boot self-test that round-trips a scratch LBA.
 *
 * Completion is polled via the CQ phase bit (no NVMe IRQ); every hardware wait
 * is spin-bounded so a missing/wedged controller can never hang the boot. PMM
 * pages are page-aligned and identity-mapped (phys == pointer), so a queue / PRP
 * page doubles as both its DMA address and a CPU pointer.
 */
#include "nvme.h"
#include "pcie.h"
#include "vmm.h"
#include "pmm.h"
#include "blk.h"
#include "console.h"
#include "irq.h"        /* DDR-774b: msix_register */
#include "lapic.h"      /* DDR-774b: lapic_id for the MSI-X message address */

/* BAR0-relative controller registers (NVMe 1.x). */
#define NVME_CAP   0x00      /* u64: MQES[15:0], DSTRD[35:32], TO[31:24] */
#define NVME_VS    0x08
#define NVME_CC    0x14
#define NVME_CSTS  0x1C
#define NVME_AQA   0x24
#define NVME_ASQ   0x28      /* u64 */
#define NVME_ACQ   0x30      /* u64 */
#define NVME_DBBASE 0x1000   /* doorbell array base */

#define CC_EN      (1u << 0)
#define CSTS_RDY   (1u << 0)

/* Admin + I/O queues are 64 entries — one 4 KiB page holds both the 64-byte SQ
 * entries and the 16-byte CQ entries. AQA / Create-Queue sizes encode depth-1. */
#define Q_DEPTH    64
#define IO_QID     1

/* Distinct high-VA window for the NVMe BAR (virtio's map_bar uses a different
 * base). Two pages cover the register block + the doorbells at 0x1000. */
#define NVME_BAR_VBASE 0xFFFFD20000000000ull
#define NVME_BAR_MAP   0x2000u

/* DDR-774b: MSI-X. The table's BAR index + offset are runtime values, so it gets
 * its own 2-page uncached window rather than growing the BAR0 register mapping
 * (2 pages because the table offset is only 8-byte aligned by spec, so a 16-byte
 * entry can straddle a page). Vector 50 is free — DDR-771 moved virtio-blk to
 * 56-63, and idt.c gates 0-63 with MSIX_VEC_COUNT=14 covering 50-63. */
#define NVME_MSIX_VBASE 0xFFFFD21000000000ull
#define NVME_MSIX_MAP   0x2000u
#define NVME_MSIX_VEC   50u
/* DDR-774c: Create I/O CQ CDW11[31:16] "Interrupt Vector" is an INDEX INTO THE
 * MSI-X TABLE, not the x86 interrupt vector. We program table entry 0 (whose
 * message-data field carries x86 vector NVME_MSIX_VEC), so the IV field must be
 * 0. Passing the x86 vector here (the DDR-774b bug) pointed the controller at an
 * unprogrammed, masked table entry — hence irqs=0. */
#define NVME_MSIX_ENTRY 0u

/* NVMe submission-queue entry (64 bytes). We fill only the fields admin/NVM
 * commands need (opcode/CID, NSID, PRP1, CDW10..12). */
struct nvme_sqe {
    uint32_t cdw0;      /* opcode[7:0] | fuse[9:8] | psdt[15:14] | CID[31:16] */
    uint32_t nsid;
    uint64_t rsvd2;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};

struct nvme_cqe {
    uint32_t cdw0;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;    /* bit0 = phase; bits 15:1 = status field */
};

struct nvme_queue {
    struct nvme_sqe *sq;
    struct nvme_cqe *cq;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint8_t  phase;     /* expected phase bit for the next CQE */
    uint16_t qid;
};

struct nvme {
    volatile uint8_t *bar;
    uint32_t dstrd;                 /* doorbell stride shift from CAP */
    struct nvme_queue admin;
    struct nvme_queue io;
    uint16_t cid;
    uint8_t  ready;                 /* controller enabled + Identify done */
    uint8_t  io_ready;              /* I/O queue up + registered */
    uint64_t prp_list;              /* DDR-772: scratch PRP-list page (512 entries) */
    uint64_t nsze;                  /* namespace size in LBAs */
    uint32_t lba_bytes;             /* logical block size */
    char     model[41];
    struct blk_device bd;
};

static struct nvme g_nvme;          /* single controller this slice */

/* DDR-774b: MSI-X completion interrupts are ARMED but nothing depends on them yet
 * — completion is still polled. The handler therefore does bounded work (one
 * increment) and deliberately does NOT touch CQ head/phase, which the polling
 * loop owns; racing it here would corrupt an in-flight command (S6). The LAPIC
 * EOI is done by the MSI-X dispatch in idt.c. DDR-774c converts the wait. */
static volatile uint32_t g_nvme_irqs;
static void nvme_msix_isr(void) { g_nvme_irqs++; }

/* Physical base of BAR `idx` (handles a 64-bit memory BAR), or 0. */
static uint64_t nvme_bar_base(uint8_t bus, uint8_t dev, uint8_t func, uint8_t idx) {
    uint16_t off = (uint16_t)(0x10 + idx * 4);
    uint32_t lo = pcie_read32(bus, dev, func, off);
    if (((lo >> 1) & 3) == 2) {                     /* 64-bit memory BAR */
        uint32_t hi = pcie_read32(bus, dev, func, (uint16_t)(off + 4));
        return (uint64_t)(lo & 0xFFFFFFF0u) | ((uint64_t)hi << 32);
    }
    return lo & 0xFFFFFFF0u;
}

/* Locate, map and program the controller's MSI-X entry 0 for NVME_MSIX_VEC.
 * Returns 0 on success. Leaves completion polling untouched (DDR-774b). */
static int nvme_msix_setup(struct nvme *n, uint8_t bus, uint8_t dev, uint8_t func) {
    (void)n;
    uint8_t bir; uint32_t table_off;
    uint8_t cap = pcie_msix_find(bus, dev, func, &bir, &table_off);
    if (!cap)
        return -1;
    uint64_t bbase = nvme_bar_base(bus, dev, func, bir);
    if (!bbase)
        return -1;

    uint64_t page = (bbase + table_off) & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint32_t off = 0; off < NVME_MSIX_MAP; off += PAGE_SIZE)
        vmm_map(NVME_MSIX_VBASE + off, page + off, VMM_RW | VMM_PCD);
    volatile uint32_t *entry0 =
        (volatile uint32_t *)(uintptr_t)(NVME_MSIX_VBASE + ((bbase + table_off) & (PAGE_SIZE - 1)));

    msix_register(NVME_MSIX_VEC, nvme_msix_isr);
    pcie_msix_program(bus, dev, func, cap, entry0, (uint8_t)NVME_MSIX_VEC, lapic_id());
    pcie_intx_disable(bus, dev, func);
    return 0;
}

static inline uint32_t rd32(struct nvme *n, uint32_t off) {
    return *(volatile uint32_t *)(n->bar + off);
}
static inline void wr32(struct nvme *n, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(n->bar + off) = v;
}
static inline uint64_t rd64(struct nvme *n, uint32_t off) {
    return *(volatile uint64_t *)(n->bar + off);
}
/* ASQ/ACQ as two 32-bit writes — controllers may latch lo then hi. */
static inline void wr64(struct nvme *n, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(n->bar + off)     = (uint32_t)v;
    *(volatile uint32_t *)(n->bar + off + 4) = (uint32_t)(v >> 32);
}

/* Doorbell offsets for a queue (2*qid = SQ tail, 2*qid+1 = CQ head). */
static inline uint32_t sq_db(struct nvme *n, uint16_t qid) {
    return NVME_DBBASE + (2u * qid) * (4u << n->dstrd);
}
static inline uint32_t cq_db(struct nvme *n, uint16_t qid) {
    return NVME_DBBASE + (2u * qid + 1u) * (4u << n->dstrd);
}

static void zero_page(void *p) {
    uint64_t *q = (uint64_t *)p;
    for (unsigned i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++)
        q[i] = 0;
}

/* Bounded busy-wait for (rd32(reg) & mask) == want. 0 on success, -1 timeout.
 * Independent of the timer (may not be ticking here). */
static int wait_reg(struct nvme *n, uint32_t reg, uint32_t mask, uint32_t want) {
    for (uint64_t i = 0; i < 50000000ull; i++) {
        if ((rd32(n, reg) & mask) == want)
            return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

/* Submit one command on queue q and poll its completion. Returns the CQE status
 * field (0 = success) or 0xFFFF on completion timeout. */
static uint16_t nvme_submit(struct nvme *n, struct nvme_queue *q, uint8_t opcode,
                            uint32_t nsid, uint64_t prp1, uint64_t prp2,
                            uint32_t cdw10, uint32_t cdw11, uint32_t cdw12) {
    struct nvme_sqe *e = &q->sq[q->sq_tail];
    for (unsigned i = 0; i < sizeof *e / sizeof(uint32_t); i++)
        ((uint32_t *)e)[i] = 0;                        /* clear the 64-byte entry */
    uint16_t cid = n->cid++;
    e->cdw0  = (uint32_t)opcode | ((uint32_t)cid << 16);
    e->nsid  = nsid;
    e->prp1  = prp1;
    e->prp2  = prp2;
    e->cdw10 = cdw10;
    e->cdw11 = cdw11;
    e->cdw12 = cdw12;

    q->sq_tail = (uint16_t)((q->sq_tail + 1) % Q_DEPTH);
    wr32(n, sq_db(n, q->qid), q->sq_tail);             /* ring SQ tail doorbell */

    struct nvme_cqe *c = &q->cq[q->cq_head];
    for (uint64_t i = 0; i < 50000000ull; i++) {
        uint16_t st = c->status;
        if ((st & 1) == q->phase) {
            uint16_t status = (uint16_t)(st >> 1);     /* strip phase bit */
            q->cq_head = (uint16_t)((q->cq_head + 1) % Q_DEPTH);
            if (q->cq_head == 0)
                q->phase ^= 1;                         /* wrapped → toggle phase */
            wr32(n, cq_db(n, q->qid), q->cq_head);     /* ring CQ head doorbell */
            return status;
        }
        __asm__ volatile("pause");
    }
    return 0xFFFF;
}

/* Allocate + init a queue's SQ/CQ pages. Returns 0 on success, -1 on no memory. */
static int queue_alloc(struct nvme_queue *q, uint16_t qid) {
    uint64_t sq_phys = pmm_alloc_page();
    uint64_t cq_phys = pmm_alloc_page();
    if (!sq_phys || !cq_phys)
        return -1;
    q->sq = (struct nvme_sqe *)(uintptr_t)sq_phys;
    q->cq = (struct nvme_cqe *)(uintptr_t)cq_phys;
    zero_page(q->sq);
    zero_page(q->cq);
    q->sq_tail = 0;
    q->cq_head = 0;
    q->phase = 1;                       /* CQEs start phase 0 → first pass sets 1 */
    q->qid = qid;
    return 0;
}

/* One NVM Read (0x02) / Write (0x01) covering `sectors` 512-byte LBAs from the
 * identity-mapped buffer at buf_phys. Each command spans at most to the next
 * 4 KiB page boundary so PRP1 alone suffices (DDR-766: no PRP list this slice).
 * Returns 0 on success, -1 on any command failure. */
static int nvme_io(struct nvme *n, int is_write, uint64_t lba,
                   uint64_t buf_phys, uint32_t sectors) {
    if (!n->io_ready)
        return -1;
    /* Cap a single command at what one PRP-list page covers: PRP1 (partial first
     * page) + 512 further pages ~= 2 MiB. Fall back to 2 pages if no list page. */
    const uint32_t max_lbas = n->prp_list ? 4096u : (2u * (PAGE_SIZE / 512u));
    while (sectors) {
        uint32_t this = (sectors > max_lbas) ? max_lbas : sectors;
        uint32_t nbytes = this * 512u;
        uint32_t off    = (uint32_t)(buf_phys & (PAGE_SIZE - 1));
        uint32_t first  = PAGE_SIZE - off;              /* bytes PRP1 covers */
        uint64_t prp2   = 0;

        if (nbytes > first) {
            uint64_t p2 = (buf_phys & ~(uint64_t)(PAGE_SIZE - 1)) + PAGE_SIZE;
            uint32_t more = (nbytes - first + PAGE_SIZE - 1) / PAGE_SIZE;
            if (more == 1) {
                prp2 = p2;                              /* one more page */
            } else {
                uint64_t *list = (uint64_t *)(uintptr_t)n->prp_list;
                for (uint32_t i = 0; i < more; i++)
                    list[i] = p2 + (uint64_t)i * PAGE_SIZE;
                prp2 = n->prp_list;                     /* PRP list for pages 2..N */
            }
        }

        uint16_t st = nvme_submit(n, &n->io, is_write ? 0x01 : 0x02, 1,
                                  buf_phys, prp2,
                                  (uint32_t)lba, (uint32_t)(lba >> 32), this - 1);
        if (st != 0)
            return -1;
        buf_phys += (uint64_t)nbytes;
        lba      += this;
        sectors  -= this;
    }
    return 0;
}

static int nvme_bd_read(struct blk_device *bd, uint64_t lba, void *buf, uint32_t count) {
    return nvme_io((struct nvme *)bd->drv, 0, lba, (uint64_t)(uintptr_t)buf, count);
}
static int nvme_bd_write(struct blk_device *bd, uint64_t lba, const void *buf, uint32_t count) {
    return nvme_io((struct nvme *)bd->drv, 1, lba, (uint64_t)(uintptr_t)buf, count);
}

/* Round-trip a scratch region through the I/O path (DDR-766 gate): write a known
 * pattern to LBA 100 (8 sectors = one 4 KiB page), read it back, and verify. Uses
 * the dedicated nvme.img — never the boot/FAT/SFS disks. */
#define NVME_SELFTEST_LBA 100u
static void nvme_selftest(struct nvme *n) {
    uint64_t wp = pmm_alloc_page();
    uint64_t rp = pmm_alloc_page();
    if (!wp || !rp) {
        if (wp) pmm_free_page(wp);
        if (rp) pmm_free_page(rp);
        return;
    }
    uint32_t *w = (uint32_t *)(uintptr_t)wp;
    uint32_t *r = (uint32_t *)(uintptr_t)rp;
    for (unsigned i = 0; i < PAGE_SIZE / 4; i++) {
        w[i] = 0xA5000000u + i;
        r[i] = 0;
    }
    int ok = nvme_io(n, 1, NVME_SELFTEST_LBA, wp, 8) == 0 &&
             nvme_io(n, 0, NVME_SELFTEST_LBA, rp, 8) == 0;
    if (ok) {
        for (unsigned i = 0; i < PAGE_SIZE / 4; i++) {
            if (r[i] != w[i]) { ok = 0; break; }
        }
    }
    kputs(ok ? "PRADYOS_NVME_RW_OK\r\n" : "PRADYOS_NVME_RW_FAIL\r\n");
    pmm_free_page(wp);
    pmm_free_page(rp);

    /* DDR-772: 16 KiB (4-page) round-trip — one command via a 3-entry PRP list. */
    uint64_t wq = pmm_alloc_pages(2);   /* 16 KiB contiguous */
    uint64_t rq = pmm_alloc_pages(2);
    if (wq && rq) {
        uint32_t *wl = (uint32_t *)(uintptr_t)wq;
        uint32_t *rl = (uint32_t *)(uintptr_t)rq;
        for (unsigned i = 0; i < (4 * PAGE_SIZE) / 4; i++) { wl[i] = 0x5EED0000u + i; rl[i] = 0; }
        int pok = nvme_io(n, 1, NVME_SELFTEST_LBA + 8, wq, 32) == 0 &&
                  nvme_io(n, 0, NVME_SELFTEST_LBA + 8, rq, 32) == 0;
        if (pok)
            for (unsigned i = 0; i < (4 * PAGE_SIZE) / 4; i++)
                if (rl[i] != wl[i]) { pok = 0; break; }
        kputs(pok ? "PRADYOS_NVME_PRP_OK\r\n" : "PRADYOS_NVME_PRP_FAIL\r\n");
    }
    if (wq) pmm_free_pages(wq, 2);
    if (rq) pmm_free_pages(rq, 2);

    /* DDR-774c (c-1): prove MSI-X completions are actually DELIVERED. Completion
     * is still polled, so the final command's interrupt may still be in flight
     * when we get here — settle with a BOUNDED spin (S2) so the assertion is
     * deterministic rather than timing-dependent. Every earlier command's
     * interrupt has long since been raised, so this normally exits immediately. */
    for (uint64_t i = 0; i < 1000000ull && g_nvme_irqs == 0; i++)
        __asm__ volatile("pause");
    kputs("[nvme] irqs="); kputdec(g_nvme_irqs); kputs("\r\n");
    kputs(g_nvme_irqs ? "PRADYOS_NVME_IRQ_OK\r\n" : "PRADYOS_NVME_IRQ_FAIL\r\n");
}

void nvme_init(uint8_t bus, uint8_t dev, uint8_t func) {
    struct nvme *n = &g_nvme;
    if (n->ready)
        return;                          /* one controller this slice */

    /* PCI command: enable memory space + bus master (DMA). */
    uint32_t cmd = pcie_read32(bus, dev, func, 0x04);
    cmd |= (1u << 1) | (1u << 2);
    pcie_write32(bus, dev, func, 0x04, cmd);

    /* BAR0 → physical base (handle a 64-bit memory BAR). */
    uint32_t lo = pcie_read32(bus, dev, func, 0x10);
    uint64_t base;
    if (((lo >> 1) & 3) == 2) {
        uint32_t hi = pcie_read32(bus, dev, func, 0x14);
        base = (uint64_t)(lo & 0xFFFFFFF0u) | ((uint64_t)hi << 32);
    } else {
        base = lo & 0xFFFFFFF0u;
    }
    if (!base) {
        kputs("[nvme] no BAR0\r\n");
        return;
    }

    for (uint32_t off = 0; off < NVME_BAR_MAP; off += PAGE_SIZE)
        vmm_map(NVME_BAR_VBASE + off, base + off, VMM_RW | VMM_PCD);
    n->bar = (volatile uint8_t *)NVME_BAR_VBASE;

    uint64_t cap = rd64(n, NVME_CAP);
    n->dstrd = (uint32_t)((cap >> 32) & 0xF);

    /* Reset: clear EN, wait for RDY=0. */
    uint32_t cc = rd32(n, NVME_CC);
    cc &= ~CC_EN;
    wr32(n, NVME_CC, cc);
    if (wait_reg(n, NVME_CSTS, CSTS_RDY, 0) < 0) {
        kputs("[nvme] reset stuck\r\n");
        return;
    }

    if (queue_alloc(&n->admin, 0) < 0) {
        kputs("[nvme] no queue mem\r\n");
        return;
    }
    n->cid = 0;

    wr32(n, NVME_AQA, ((Q_DEPTH - 1) << 16) | (Q_DEPTH - 1));
    wr64(n, NVME_ASQ, (uint64_t)(uintptr_t)n->admin.sq);
    wr64(n, NVME_ACQ, (uint64_t)(uintptr_t)n->admin.cq);

    /* Enable: IOCQES=4 (16B), IOSQES=6 (64B), MPS=0 (4 KiB), CSS=0 (NVM). */
    cc = (4u << 20) | (6u << 16) | (0u << 7) | (0u << 4) | CC_EN;
    wr32(n, NVME_CC, cc);
    if (wait_reg(n, NVME_CSTS, CSTS_RDY, CSTS_RDY) < 0) {
        kputs("[nvme] controller not ready\r\n");
        return;
    }

    uint64_t data_phys = pmm_alloc_page();
    if (!data_phys) {
        kputs("[nvme] no data mem\r\n");
        return;
    }
    uint8_t *data = (uint8_t *)(uintptr_t)data_phys;

    /* Identify Controller (opcode 0x06, CNS=1). Model = bytes 24..63. */
    zero_page(data);
    uint16_t st = nvme_submit(n, &n->admin, 0x06, 0, data_phys, 0, 1, 0, 0);
    if (st != 0) {
        kputs("[nvme] identify-ctrl failed status="); kputhex(st); kputs("\r\n");
        return;
    }
    for (int i = 0; i < 40; i++)
        n->model[i] = (char)data[24 + i];
    n->model[40] = 0;
    for (int i = 39; i >= 0 && n->model[i] == ' '; i--)
        n->model[i] = 0;

    /* Identify Namespace (CNS=0, NSID=1). NSZE @0 (u64); LBA size from the active
     * format lbaf[FLBAS&0xf], LBADS = bits 16..23 → 2^LBADS bytes. */
    zero_page(data);
    st = nvme_submit(n, &n->admin, 0x06, 1, data_phys, 0, 0, 0, 0);
    if (st != 0) {
        kputs("[nvme] identify-ns failed status="); kputhex(st); kputs("\r\n");
        return;
    }
    n->nsze = *(uint64_t *)&data[0];
    uint8_t flbas = data[26] & 0xF;
    uint32_t lbaf = *(uint32_t *)&data[128 + 4 * flbas];
    uint32_t lbads = (lbaf >> 16) & 0xFF;
    n->lba_bytes = (lbads < 32) ? (1u << lbads) : 512u;

    n->ready = 1;
    kputs("[nvme] ");
    kputs(n->model[0] ? n->model : "(unknown)");
    kputs(" ns1 ");
    kputdec(n->nsze);
    kputs(" LBAs x ");
    kputdec(n->lba_bytes);
    kputs(" B\r\n");

    /* DDR-766: create one I/O queue pair and register the block device. The
     * block layer is 512-byte-sector; support only 512-byte LBAs this slice. */
    if (n->lba_bytes != 512) {
        kputs("[nvme] LBA size != 512, block I/O skipped\r\n");
        return;
    }
    if (queue_alloc(&n->io, IO_QID) < 0) {
        kputs("[nvme] no io-queue mem\r\n");
        return;
    }
    /* DDR-774b: arm MSI-X before creating the CQ, so the CQ can be created with
     * IEN + vector in one shot. Failure is non-fatal — the controller simply
     * stays in the (unchanged) polled mode. */
    int msix = (nvme_msix_setup(n, bus, dev, func) == 0);
    if (msix) {
        { kline k; kline_init(&k);                   /* DDR-1055 */
          kline_s(&k, "[nvme] msix vec=");
          kline_d(&k, NVME_MSIX_VEC);
          kline_s(&k, "\r\n"); kline_emit(&k); }
    } else {
        kputs("[nvme] msix unavailable, polling only\r\n");
    }

    /* Create I/O Completion Queue (admin 0x05): PC=1, plus IEN + interrupt vector
     * (cdw11 bit 1 and bits 31:16) once MSI-X is armed. Completion is still
     * POLLED in nvme_submit — DDR-774c converts the wait. */
    uint32_t cq_cdw11 = 1u /*PC*/;
    if (msix)
        cq_cdw11 |= (1u << 1) | ((uint32_t)NVME_MSIX_ENTRY << 16); /* IEN | IV */
    st = nvme_submit(n, &n->admin, 0x05, 0, (uint64_t)(uintptr_t)n->io.cq, 0,
                     ((Q_DEPTH - 1) << 16) | IO_QID, cq_cdw11, 0);
    if (st != 0) {
        kputs("[nvme] create-iocq failed status="); kputhex(st); kputs("\r\n");
        return;
    }
    /* Create I/O Submission Queue (admin 0x01): PC=1, CQID=IO_QID. */
    st = nvme_submit(n, &n->admin, 0x01, 0, (uint64_t)(uintptr_t)n->io.sq, 0,
                     ((Q_DEPTH - 1) << 16) | IO_QID, 1u | ((uint32_t)IO_QID << 16), 0);
    if (st != 0) {
        kputs("[nvme] create-iosq failed status="); kputhex(st); kputs("\r\n");
        return;
    }
    n->prp_list = pmm_alloc_page();     /* DDR-772: scratch PRP-list page (>2-page I/O) */
    n->io_ready = 1;

    n->bd.name = "nvme0";
    n->bd.capacity_sectors = n->nsze;   /* 512-byte LBAs == block-layer sectors */
    n->bd.read = nvme_bd_read;
    n->bd.write = nvme_bd_write;
    n->bd.drv = n;
    blk_register(&n->bd);
    kputs("[nvme] registered nvme0 (");
    kputdec(n->nsze);
    kputs(" sectors)\r\n");

    nvme_selftest(n);
}
