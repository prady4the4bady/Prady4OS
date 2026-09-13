/* kernel/drivers/ahci/ahci.c — AHCI (SATA) block driver, Group 4 item 23,
 * DDR-875.
 *
 * SCOPE, stated first so it is not over-read: this drives PIO-free FIS-based
 * DMA reads and writes on the ports the HBA reports as having a SATA disk
 * attached, and registers each as a blk_device. It does NOT implement NCQ
 * (queued commands), port multipliers, ATAPI, or hot-plug. Those are real AHCI
 * features and their absence is a scope decision, not an oversight — nothing in
 * this queue needs them, and each adds a failure mode that would need its own
 * gate.
 *
 * ---------------------------------------------------------------------------
 * THE THINGS THAT MAKE AHCI EASY TO GET SUBTLY WRONG
 *
 * 1. STOP BEFORE YOU TOUCH. A port's command list and FIS base registers may
 *    only be written while the port engine is stopped (PxCMD.ST=0 and
 *    PxCMD.FRE=0) AND has acknowledged the stop (PxCMD.CR=0, PxCMD.FR=0).
 *    Writing them on a running port is undefined: the HBA may keep using the
 *    old addresses, so the driver reads a buffer the disk never wrote and calls
 *    it data. Every setup path here stops and WAITS for the acknowledgement.
 *
 * 2. 1 KiB AND 256-BYTE ALIGNMENT ARE ARCHITECTURAL. The command list must be
 *    1 KiB aligned and the received-FIS area 256-byte aligned. The registers
 *    simply ignore the low bits, so a misaligned allocation does not fail — it
 *    silently points the HBA at a different address than the driver thinks,
 *    which is the same corruption as (1) wearing a different hat. Both areas
 *    come from whole PMM pages, which satisfies both bounds by construction.
 *
 * 3. THE BUSY WAIT IS BOUNDED. Every hardware poll has an iteration cap. An
 *    unbounded `while (reg & BUSY)` on a wedged controller hangs the boot with
 *    no message, which is indistinguishable from a hang anywhere else.
 */
#include "ahci.h"
#include "pcie.h"
#include "blk.h"
#include "pmm.h"
#include "vmm.h"
#include "console.h"
#include "string.h"

/* ---- HBA memory registers (offsets in the ABAR window) ------------------ */
#define HBA_CAP         0x00
#define HBA_GHC         0x04
#define HBA_PI          0x0C        /* ports implemented bitmap */
#define HBA_PORT_BASE   0x100
#define HBA_PORT_SIZE   0x80

#define GHC_AE          (1u << 31)  /* AHCI enable */
#define GHC_HR          (1u << 0)   /* HBA reset   */

/* ---- per-port registers ------------------------------------------------- */
#define PxCLB           0x00
#define PxCLBU          0x04
#define PxFB            0x08
#define PxFBU           0x0C
#define PxIS            0x10
#define PxIE            0x14
#define PxCMD           0x18
#define PxTFD           0x20
#define PxSIG           0x24
#define PxSSTS          0x28
#define PxCI            0x38

#define PxCMD_ST        (1u << 0)
#define PxCMD_FRE       (1u << 4)
#define PxCMD_FR        (1u << 14)
#define PxCMD_CR        (1u << 15)

#define TFD_BSY         (1u << 7)
#define TFD_DRQ         (1u << 3)
#define TFD_ERR         (1u << 0)

#define SIG_SATA        0x00000101u /* a plain SATA disk (not ATAPI/SEMB/PM) */

#define ATA_READ_DMA_EX  0x25
#define ATA_WRITE_DMA_EX 0x35
#define ATA_IDENTIFY     0xEC

#define AHCI_MAX_PORTS  32
#define AHCI_MAX_DISKS  4
#define SECTOR          512

/* Bounded polls. At QEMU speeds these complete in a handful of iterations; the
 * cap exists so a wedged controller reports itself instead of hanging boot. */
#define SPIN_LIMIT      1000000u

struct hba_cmd_header {
    uint16_t flags;             /* CFL(4:0) | A | W | P ... */
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
};

struct hba_prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;               /* bit 31 = interrupt on completion */
};

struct hba_cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct hba_prdt_entry prdt[8];
};

struct ahci_disk {
    volatile uint8_t *port;     /* MMIO base of this port's register block */
    struct hba_cmd_header *clb; /* command list (1 KiB aligned)            */
    struct hba_cmd_table  *ctb; /* command table for slot 0                */
    struct blk_device bd;
};

static struct ahci_disk g_disks[AHCI_MAX_DISKS];
static unsigned g_ndisks;

unsigned ahci_port_count(void) { return g_ndisks; }

static inline uint32_t rd(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}
static inline void wr(volatile uint8_t *base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(base + off) = v;
}

/* Stop the port engine and WAIT for the HBA to acknowledge. Returns 0 on
 * success. See note (1): writing CLB/FB before this completes is the classic
 * silent-corruption bug. */
static int port_stop(volatile uint8_t *p) {
    uint32_t cmd = rd(p, PxCMD);
    cmd &= ~(PxCMD_ST | PxCMD_FRE);
    wr(p, PxCMD, cmd);
    for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
        uint32_t c = rd(p, PxCMD);
        if (!(c & PxCMD_CR) && !(c & PxCMD_FR))
            return 0;
    }
    return -1;
}

static void port_start(volatile uint8_t *p) {
    for (uint32_t i = 0; i < SPIN_LIMIT; i++)
        if (!(rd(p, PxCMD) & PxCMD_CR))
            break;
    uint32_t cmd = rd(p, PxCMD);
    wr(p, PxCMD, cmd | PxCMD_FRE);
    wr(p, PxCMD, rd(p, PxCMD) | PxCMD_ST);
}

/* Issue the command already built in slot 0 and wait for it to retire. */
static int port_run(volatile uint8_t *p) {
    for (uint32_t i = 0; i < SPIN_LIMIT; i++)
        if (!(rd(p, PxTFD) & (TFD_BSY | TFD_DRQ)))
            break;

    wr(p, PxIS, 0xFFFFFFFFu);        /* clear stale status before issuing */
    wr(p, PxCI, 1u);                 /* slot 0 */

    for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
        if (!(rd(p, PxCI) & 1u))
            break;
        if (rd(p, PxTFD) & TFD_ERR)
            return -1;
    }
    if (rd(p, PxCI) & 1u)
        return -1;                   /* never retired within the bound */
    if (rd(p, PxTFD) & TFD_ERR)
        return -1;
    return 0;
}

/* Build the slot-0 command: a 5-dword H2D register FIS plus one PRDT entry.
 * `write` selects the direction bit in the command header. */
static int issue(struct ahci_disk *d, uint8_t cmd, uint64_t lba,
                 uint64_t buf_phys, uint32_t sectors, int write) {
    memset(d->ctb, 0, sizeof *d->ctb);

    uint8_t *f = d->ctb->cfis;
    f[0] = 0x27;                     /* FIS type: Register Host-to-Device */
    f[1] = 0x80;                     /* C bit: this is a command          */
    f[2] = cmd;
    f[4] = (uint8_t)(lba & 0xFF);
    f[5] = (uint8_t)((lba >> 8) & 0xFF);
    f[6] = (uint8_t)((lba >> 16) & 0xFF);
    f[7] = 0x40;                     /* LBA mode */
    f[8] = (uint8_t)((lba >> 24) & 0xFF);
    f[9] = (uint8_t)((lba >> 32) & 0xFF);
    f[10] = (uint8_t)((lba >> 40) & 0xFF);
    f[12] = (uint8_t)(sectors & 0xFF);
    f[13] = (uint8_t)((sectors >> 8) & 0xFF);

    d->ctb->prdt[0].dba  = (uint32_t)(buf_phys & 0xFFFFFFFFu);
    d->ctb->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
    /* dbc is a BYTE COUNT MINUS ONE. Writing the plain count transfers one
     * byte too many — which for a 512-byte sector overruns the caller's
     * buffer by exactly one byte, the kind of damage that shows up far away. */
    d->ctb->prdt[0].dbc  = (sectors * SECTOR) - 1;

    d->clb[0].flags = (uint16_t)(5 | (write ? (1u << 6) : 0));  /* CFL=5 dwords */
    d->clb[0].prdtl = 1;
    d->clb[0].prdbc = 0;

    return port_run(d->port);
}

static int ahci_read(struct blk_device *bd, uint64_t lba, void *buf,
                     uint32_t count) {
    struct ahci_disk *d = (struct ahci_disk *)bd->drv;
    return issue(d, ATA_READ_DMA_EX, lba, (uint64_t)(uintptr_t)buf, count, 0);
}

static int ahci_write(struct blk_device *bd, uint64_t lba, const void *buf,
                      uint32_t count) {
    struct ahci_disk *d = (struct ahci_disk *)bd->drv;
    return issue(d, ATA_WRITE_DMA_EX, lba, (uint64_t)(uintptr_t)buf, count, 1);
}

static void port_setup(volatile uint8_t *pbase) {
    if (g_ndisks >= AHCI_MAX_DISKS)
        return;

    /* Only a plain SATA disk with an established link. SIG distinguishes disk
     * from ATAPI/enclosure/port-multiplier, and DET==3 means the PHY actually
     * negotiated — a port can be implemented and empty. */
    if ((rd(pbase, PxSSTS) & 0x0F) != 3)
        return;
    if (rd(pbase, PxSIG) != SIG_SATA)
        return;

    if (port_stop(pbase) != 0) {
        kputs("[ahci] port would not stop; skipping\r\n");
        return;
    }

    /* Whole pages: satisfies the 1 KiB (command list) and 256-byte (FIS)
     * alignment requirements by construction — see note (2). */
    uint64_t clb_pg = pmm_alloc_page();
    uint64_t fis_pg = pmm_alloc_page();
    uint64_t ctb_pg = pmm_alloc_page();
    if (!clb_pg || !fis_pg || !ctb_pg) {
        kputs("[ahci] out of memory for port structures\r\n");
        return;
    }
    memset((void *)(uintptr_t)clb_pg, 0, 4096);
    memset((void *)(uintptr_t)fis_pg, 0, 4096);
    memset((void *)(uintptr_t)ctb_pg, 0, 4096);

    struct ahci_disk *d = &g_disks[g_ndisks];
    d->port = pbase;
    d->clb  = (struct hba_cmd_header *)(uintptr_t)clb_pg;
    d->ctb  = (struct hba_cmd_table *)(uintptr_t)ctb_pg;

    d->clb[0].ctba  = (uint32_t)(ctb_pg & 0xFFFFFFFFu);
    d->clb[0].ctbau = (uint32_t)(ctb_pg >> 32);

    wr(pbase, PxCLB,  (uint32_t)(clb_pg & 0xFFFFFFFFu));
    wr(pbase, PxCLBU, (uint32_t)(clb_pg >> 32));
    wr(pbase, PxFB,   (uint32_t)(fis_pg & 0xFFFFFFFFu));
    wr(pbase, PxFBU,  (uint32_t)(fis_pg >> 32));
    wr(pbase, PxIE, 0);              /* polled; no interrupt wanted */

    port_start(pbase);

    /* IDENTIFY gives the sector count. Without it the capacity would have to be
     * guessed, and a wrong capacity lets the filesystem address past the end. */
    uint64_t id_pg = pmm_alloc_page();
    if (!id_pg)
        return;
    memset((void *)(uintptr_t)id_pg, 0, 4096);
    if (issue(d, ATA_IDENTIFY, 0, id_pg, 1, 0) != 0) {
        kputs("[ahci] IDENTIFY failed; port skipped\r\n");
        pmm_free_page(id_pg);
        return;
    }

    const uint16_t *id = (const uint16_t *)(uintptr_t)id_pg;
    uint64_t sectors = ((uint64_t)id[103] << 48) | ((uint64_t)id[102] << 32) |
                       ((uint64_t)id[101] << 16) |  (uint64_t)id[100];
    if (sectors == 0)                /* pre-48-bit disk: words 60-61 */
        sectors = ((uint64_t)id[61] << 16) | id[60];
    pmm_free_page(id_pg);

    d->bd.name = "ahci";
    d->bd.capacity_sectors = sectors;
    d->bd.read  = ahci_read;
    d->bd.write = ahci_write;
    d->bd.drv   = d;
    blk_register(&d->bd);
    g_ndisks++;

    { kline k; kline_init(&k);                       /* DDR-1055 */
      kline_s(&k, "[ahci] port disk, sectors=");
      kline_d(&k, sectors);
      kline_s(&k, "\r\n"); kline_emit(&k); }
}

void ahci_init(uint8_t bus, uint8_t dev, uint8_t func) {
    /* ABAR is BAR5 for AHCI. The low 4 bits are flags, not address. */
    uint32_t bar5 = pcie_read32(bus, dev, func, 0x24) & ~0xFu;
    if (!bar5)
        return;

    /* Bus-master enable: without it the HBA cannot DMA, every command times out
     * and the disk looks broken rather than unconfigured. */
    uint32_t cmdreg = pcie_read32(bus, dev, func, 0x04);
    pcie_write32(bus, dev, func, 0x04, cmdreg | (1u << 2) | (1u << 1));

    volatile uint8_t *abar = (volatile uint8_t *)(uintptr_t)bar5;
    if (vmm_map((uint64_t)bar5, (uint64_t)bar5, VMM_RW | VMM_PCD) != 0) {
        kputs("[ahci] cannot map ABAR\r\n");
        return;
    }

    wr(abar, HBA_GHC, rd(abar, HBA_GHC) | GHC_AE);

    uint32_t pi = rd(abar, HBA_PI);
    for (unsigned i = 0; i < AHCI_MAX_PORTS; i++) {
        if (!(pi & (1u << i)))
            continue;
        port_setup(abar + HBA_PORT_BASE + i * HBA_PORT_SIZE);
    }

    kputs("[ahci] controller ready, disks=");
    kputdec(g_ndisks);
    kputs("\r\n");
}
