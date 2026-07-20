/* kernel/drivers/nvme/nvme.c — NVMe controller bring-up + Identify (DDR-765).
 *
 * First of two NVMe slices. Detects one NVMe controller (PCIe class 0x01 /
 * subclass 0x08), maps BAR0 uncached, enables the controller, stands up a
 * single admin submission/completion queue pair, and issues Identify Controller
 * + Identify Namespace to read the model number and namespace geometry. There is
 * no block I/O here and no blk_register — that is DDR-766. Completion is polled
 * via the CQ phase bit (no NVMe IRQ this slice); every hardware wait is bounded
 * by a spin counter so a missing or wedged controller can never hang the boot.
 *
 * PMM pages are page-aligned and identity-mapped (phys == pointer), so an admin
 * queue / PRP page doubles as both its DMA address and a CPU pointer.
 */
#include "nvme.h"
#include "pcie.h"
#include "vmm.h"
#include "pmm.h"
#include "console.h"

/* BAR0-relative controller registers (NVMe 1.x). */
#define NVME_CAP   0x00      /* u64: MQES[15:0], DSTRD[35:32], TO[31:24] */
#define NVME_VS    0x08
#define NVME_CC    0x14
#define NVME_CSTS  0x1C
#define NVME_AQA   0x24
#define NVME_ASQ   0x28      /* u64 */
#define NVME_ACQ   0x30      /* u64 */
#define NVME_SQ0TDBL 0x1000  /* admin SQ tail doorbell; CQ head at +stride */

#define CC_EN      (1u << 0)
#define CSTS_RDY   (1u << 0)

/* Admin queue depth: 64 entries fits one 4 KiB page for both the 64-byte SQ
 * entries and the 16-byte CQ entries (16*256 also fits, but 64 keeps them
 * symmetric and one page each). AQA encodes size-1. */
#define AQ_DEPTH   64

/* Distinct high-VA window for the NVMe BAR (virtio's map_bar uses a different
 * base). Two pages cover the register block + the admin doorbells at 0x1000. */
#define NVME_BAR_VBASE 0xFFFFD20000000000ull
#define NVME_BAR_MAP   0x2000u

/* One NVMe submission-queue entry (64 bytes) as raw dwords; we only fill the
 * fields Identify needs (opcode/CID, NSID, PRP1, CDW10). */
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
    uint16_t status;    /* phase bit is bit 0 here (P[16] of the DW3 word) */
};

struct nvme {
    volatile uint8_t *bar;
    uint32_t dstrd;                 /* doorbell stride shift from CAP */
    struct nvme_sqe *asq;           /* admin submission queue (one page) */
    struct nvme_cqe *acq;           /* admin completion queue (one page) */
    uint16_t sq_tail;
    uint16_t cq_head;
    uint8_t  cq_phase;              /* expected phase bit for the next CQE */
    uint16_t cid;
    uint8_t  ready;                 /* controller enabled + Identify done */
    uint64_t nsze;                  /* namespace size in LBAs */
    uint32_t lba_bytes;             /* logical block size */
    char     model[41];
};

static struct nvme g_nvme;          /* single controller this slice */

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

static void zero_page(void *p) {
    uint64_t *q = (uint64_t *)p;
    for (unsigned i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++)
        q[i] = 0;
}

/* Bounded busy-wait for (rd32(reg) & mask) == want. Returns 0 on success, -1 on
 * timeout. Independent of the timer (may not be ticking here). */
static int wait_reg(struct nvme *n, uint32_t reg, uint32_t mask, uint32_t want) {
    for (uint64_t i = 0; i < 50000000ull; i++) {
        if ((rd32(n, reg) & mask) == want)
            return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

/* Submit one admin command and poll its completion. Returns the CQE status
 * field (0 = success) or 0xFFFF on completion timeout. */
static uint16_t admin_cmd(struct nvme *n, uint8_t opcode, uint32_t nsid,
                          uint64_t prp1, uint32_t cdw10) {
    struct nvme_sqe *e = &n->asq[n->sq_tail];
    for (unsigned i = 0; i < sizeof *e / sizeof(uint32_t); i++)
        ((uint32_t *)e)[i] = 0;                        /* clear the 64-byte entry */
    uint16_t cid = n->cid++;
    e->cdw0  = (uint32_t)opcode | ((uint32_t)cid << 16);
    e->nsid  = nsid;
    e->prp1  = prp1;
    e->cdw10 = cdw10;

    n->sq_tail = (uint16_t)((n->sq_tail + 1) % AQ_DEPTH);
    uint32_t stride = 4u << n->dstrd;
    wr32(n, NVME_SQ0TDBL, n->sq_tail);                 /* ring SQ tail doorbell */

    /* Poll the CQ head for a phase-bit flip. */
    struct nvme_cqe *c = &n->acq[n->cq_head];
    for (uint64_t i = 0; i < 50000000ull; i++) {
        uint16_t st = c->status;
        if ((st & 1) == n->cq_phase) {
            uint16_t status = (uint16_t)(st >> 1);     /* strip phase bit */
            n->cq_head = (uint16_t)((n->cq_head + 1) % AQ_DEPTH);
            if (n->cq_head == 0)
                n->cq_phase ^= 1;                      /* wrapped → toggle phase */
            wr32(n, NVME_SQ0TDBL + stride, n->cq_head);/* ring CQ head doorbell */
            return status;
        }
        __asm__ volatile("pause");
    }
    return 0xFFFF;
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

    /* Map BAR0 uncached. */
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

    /* Admin queue pages (identity-mapped → phys == pointer). */
    uint64_t sq_phys = pmm_alloc_page();
    uint64_t cq_phys = pmm_alloc_page();
    if (!sq_phys || !cq_phys) {
        kputs("[nvme] no queue mem\r\n");
        return;
    }
    n->asq = (struct nvme_sqe *)(uintptr_t)sq_phys;
    n->acq = (struct nvme_cqe *)(uintptr_t)cq_phys;
    zero_page(n->asq);
    zero_page(n->acq);
    n->sq_tail = 0;
    n->cq_head = 0;
    n->cq_phase = 1;                     /* CQEs start phase 0 → first pass sets 1 */
    n->cid = 0;

    wr32(n, NVME_AQA, ((AQ_DEPTH - 1) << 16) | (AQ_DEPTH - 1));
    wr64(n, NVME_ASQ, sq_phys);
    wr64(n, NVME_ACQ, cq_phys);

    /* Enable: IOCQES=4 (16B), IOSQES=6 (64B), MPS=0 (4 KiB), CSS=0 (NVM). */
    cc = (4u << 20) | (6u << 16) | (0u << 7) | (0u << 4) | CC_EN;
    wr32(n, NVME_CC, cc);
    if (wait_reg(n, NVME_CSTS, CSTS_RDY, CSTS_RDY) < 0) {
        kputs("[nvme] controller not ready\r\n");
        return;
    }

    /* PRP data page for Identify results. */
    uint64_t data_phys = pmm_alloc_page();
    if (!data_phys) {
        kputs("[nvme] no data mem\r\n");
        return;
    }
    uint8_t *data = (uint8_t *)(uintptr_t)data_phys;

    /* Identify Controller (opcode 0x06, CNS=1). Model number = bytes 24..63. */
    zero_page(data);
    uint16_t st = admin_cmd(n, 0x06, 0, data_phys, 1);
    if (st != 0) {
        kputs("[nvme] identify-ctrl failed status="); kputhex(st); kputs("\r\n");
        return;
    }
    for (int i = 0; i < 40; i++)
        n->model[i] = (char)data[24 + i];
    n->model[40] = 0;
    /* Trim trailing spaces (NVMe pads the field). */
    for (int i = 39; i >= 0 && n->model[i] == ' '; i--)
        n->model[i] = 0;

    /* Identify Namespace (CNS=0, NSID=1). NSZE @0 (u64); LBA size from the
     * active format lbaf[FLBAS&0xf], LBADS = bits 16..23 → 2^LBADS bytes. */
    zero_page(data);
    st = admin_cmd(n, 0x06, 1, data_phys, 0);
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
}
