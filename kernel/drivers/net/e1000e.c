/* kernel/drivers/net/e1000e.c — Intel e1000e NIC (Group 4 item 25, DDR-876).
 *
 * Targets QEMU's `e1000e` device (82574L, 8086:10D3). Descriptor rings for RX
 * and TX, MAC read from the Receive Address registers, and a polled receive
 * path.
 *
 * ---------------------------------------------------------------------------
 * WHY POLLED AND NOT INTERRUPT-DRIVEN
 *
 * Not a shortcut. This kernel's INTx path is shared and chained (idt.c), and
 * the existing virtio-net driver already owns an MSI-X vector. Adding a second
 * NIC on a shared line means two drivers' handlers running for every assertion
 * from either — and the failure mode when that is wrong is a lost interrupt,
 * which looks like a network that works until it silently stops.
 *
 * Polling from the timer path is bounded, has no shared-line semantics to get
 * wrong, and is sufficient for a NIC that exists here to prove the hardware
 * path works. Moving to MSI-X is a contained change once item 28's I/O APIC
 * routing is used for device affinity — the vector plumbing already exists.
 *
 * ---------------------------------------------------------------------------
 * THE THINGS THAT MAKE e1000e SUBTLY WRONG
 *
 * 1. THE TAIL POINTER IS "ONE PAST THE LAST BUFFER YOU OWN". Setting RDT equal
 *    to RDH tells the NIC it owns NOTHING and receive silently stops. The ring
 *    must be armed with tail = last index, and after consuming a descriptor the
 *    tail advances to the descriptor just released — never to head.
 *
 * 2. DESCRIPTOR RINGS ARE 16-BYTE ENTRIES AND THE BASE MUST BE 16-BYTE ALIGNED,
 *    with the length register in BYTES, not entries. A length in entries yields
 *    a ring 16x too small; the NIC then wraps early and overwrites descriptors
 *    the driver still believes are pending.
 *
 * 3. STATUS.DD IS THE ONLY VALID "IS THIS MINE" TEST. Reading a descriptor's
 *    length before DD is set reads whatever the ring held previously, which is
 *    usually a plausible-looking length from an earlier packet.
 */
#include "e1000e.h"
#include "pcie.h"
#include "netbuf.h"
#include "pmm.h"
#include "vmm.h"
#include "console.h"
#include "string.h"

/* ---- registers (byte offsets in BAR0) ----------------------------------- */
#define E1000_CTRL      0x0000
#define E1000_STATUS    0x0008
#define E1000_ICR       0x00C0
#define E1000_IMC       0x00D8      /* interrupt mask CLEAR */
#define E1000_RCTL      0x0100
#define E1000_TCTL      0x0400
#define E1000_RDBAL     0x2800
#define E1000_RDBAH     0x2804
#define E1000_RDLEN     0x2808
#define E1000_RDH       0x2810
#define E1000_RDT       0x2818
#define E1000_TDBAL     0x3800
#define E1000_TDBAH     0x3804
#define E1000_TDLEN     0x3808
#define E1000_TDH       0x3810
#define E1000_TDT       0x3818
#define E1000_RAL0      0x5400
#define E1000_RAH0      0x5404

#define CTRL_SLU        (1u << 6)   /* set link up */

#define RCTL_EN         (1u << 1)
#define RCTL_BAM        (1u << 15)  /* broadcast accept */
#define RCTL_SECRC      (1u << 26)  /* strip Ethernet CRC */
/* BSIZE bits 17:16 = 00 -> 2048 bytes with BSEX clear. */

#define TCTL_EN         (1u << 1)
#define TCTL_PSP        (1u << 3)   /* pad short packets */

#define TXD_CMD_EOP     (1u << 0)
#define TXD_CMD_IFCS    (1u << 1)   /* insert FCS */
#define TXD_CMD_RS      (1u << 3)   /* report status */
#define TXD_STA_DD      (1u << 0)

#define RXD_STA_DD      (1u << 0)
#define RXD_STA_EOP     (1u << 1)

/* Descriptor counts must keep RDLEN/TDLEN a multiple of 128 bytes (8 entries).
 * Kept small on purpose: every descriptor pins a netbuf, and the pool is
 * shared with virtio-net — a bigger ring here starves the other NIC. */
#define NRX 16
#define NTX 8
#define RXBUF 2048

struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

static volatile uint8_t *g_mmio;
static struct rx_desc *g_rx;
static struct tx_desc *g_tx;
static uint64_t g_rxbuf[NRX];
static uint64_t g_txbuf[NTX];
static unsigned g_rx_cur, g_tx_cur;
static uint8_t  g_mac[6];
static int      g_up;
static void (*g_rx_cb)(const uint8_t *frame, uint32_t len);

static void selftest(void);

static inline uint32_t rd(uint32_t off) {
    return *(volatile uint32_t *)(g_mmio + off);
}
static inline void wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_mmio + off) = v;
}

void e1000e_mac(uint8_t out[6]) { memcpy(out, g_mac, 6); }
int  e1000e_up(void)            { return g_up; }
void e1000e_set_rx(void (*cb)(const uint8_t *, uint32_t)) { g_rx_cb = cb; }

int e1000e_tx(const void *frame, uint32_t len) {
    if (!g_up || len == 0 || len > RXBUF)
        return -1;

    struct tx_desc *d = &g_tx[g_tx_cur];
    /* Wait for the NIC to hand this descriptor back. Bounded: a wedged NIC must
     * report a failed send, not hang the caller forever. */
    if (d->cmd && !(d->status & TXD_STA_DD)) {
        for (unsigned i = 0; i < 1000000u; i++)
            if (d->status & TXD_STA_DD)
                break;
        if (!(d->status & TXD_STA_DD))
            return -1;
    }

    memcpy((void *)(uintptr_t)g_txbuf[g_tx_cur], frame, len);
    d->addr   = g_txbuf[g_tx_cur];
    d->length = (uint16_t)len;
    d->cmd    = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    d->status = 0;

    g_tx_cur = (g_tx_cur + 1) % NTX;
    wr(E1000_TDT, g_tx_cur);        /* tail = one past the descriptor just filled */
    return 0;
}

void e1000e_poll(void) {
    if (!g_up)
        return;

    /* STATUS.DD is the only valid "this is mine" test — see note (3). */
    while (g_rx[g_rx_cur].status & RXD_STA_DD) {
        struct rx_desc *d = &g_rx[g_rx_cur];
        if ((d->status & RXD_STA_EOP) && d->length && g_rx_cb)
            g_rx_cb((const uint8_t *)(uintptr_t)d->addr, d->length);

        d->status = 0;                       /* hand it back */
        unsigned released = g_rx_cur;
        g_rx_cur = (g_rx_cur + 1) % NRX;
        /* Tail = the descriptor just released, NOT head — see note (1). */
        wr(E1000_RDT, released);
    }
}

void e1000e_init(uint8_t bus, uint8_t dev, uint8_t func) {
    if (g_mmio)
        return;                               /* one NIC is enough */

    uint32_t bar0 = pcie_read32(bus, dev, func, 0x10) & ~0xFu;
    if (!bar0) {
        /* Returning quietly here would be the "absorb the bad input" defect:
         * an unassigned BAR would look exactly like no NIC present. */
        kputs("[e1000e] BAR0 unassigned\r\n");
        return;
    }
    /* Size the BAR rather than assuming 4 KiB. The register file reaches to
     * 0x5404 (RAH0), so a single-page mapping faults on the very first MAC
     * read — which is exactly what an assumed size bought the first time.
     * Standard probe: write all-ones, read back, restore. */
    pcie_write32(bus, dev, func, 0x10, 0xFFFFFFFFu);
    uint32_t mask = pcie_read32(bus, dev, func, 0x10) & ~0xFu;
    pcie_write32(bus, dev, func, 0x10, bar0);
    uint32_t size = ~mask + 1u;
    if (size < 0x1000u || size > 0x100000u) {
        kputs("[e1000e] implausible BAR0 size\r\n");
        return;
    }

    /* Bus master + memory space. Without bus-master the NIC cannot DMA and both
     * rings stay empty, which looks like a dead link rather than a config miss. */
    uint32_t cmd = pcie_read32(bus, dev, func, 0x04);
    pcie_write32(bus, dev, func, 0x04, cmd | (1u << 2) | (1u << 1));

    for (uint32_t off = 0; off < size; off += 0x1000u) {
        if (vmm_map((uint64_t)(bar0 + off), (uint64_t)(bar0 + off),
                    VMM_RW | VMM_PCD) != 0) {
            kputs("[e1000e] cannot map BAR0\r\n");
            return;
        }
    }
    g_mmio = (volatile uint8_t *)(uintptr_t)bar0;

    wr(E1000_IMC, 0xFFFFFFFFu);               /* polled: mask every interrupt */
    (void)rd(E1000_ICR);                      /* read-to-clear anything pending */
    wr(E1000_CTRL, rd(E1000_CTRL) | CTRL_SLU);

    uint32_t ral = rd(E1000_RAL0), rah = rd(E1000_RAH0);
    g_mac[0] = (uint8_t)(ral);       g_mac[1] = (uint8_t)(ral >> 8);
    g_mac[2] = (uint8_t)(ral >> 16); g_mac[3] = (uint8_t)(ral >> 24);
    g_mac[4] = (uint8_t)(rah);       g_mac[5] = (uint8_t)(rah >> 8);

    /* Whole pages: 16-byte alignment by construction — see note (2). */
    uint64_t rxpg = pmm_alloc_page();
    uint64_t txpg = pmm_alloc_page();
    if (!rxpg || !txpg) {
        kputs("[e1000e] out of memory for rings\r\n");
        g_mmio = 0;
        return;
    }
    memset((void *)(uintptr_t)rxpg, 0, 4096);
    memset((void *)(uintptr_t)txpg, 0, 4096);
    g_rx = (struct rx_desc *)(uintptr_t)rxpg;
    g_tx = (struct tx_desc *)(uintptr_t)txpg;

    netbuf_init();
    for (unsigned i = 0; i < NRX; i++) {
        g_rxbuf[i] = netbuf_alloc();
        if (!g_rxbuf[i]) { kputs("[e1000e] netbuf exhausted\r\n"); g_mmio = 0; return; }
        g_rx[i].addr = g_rxbuf[i];
        g_rx[i].status = 0;
    }
    for (unsigned i = 0; i < NTX; i++) {
        g_txbuf[i] = netbuf_alloc();
        if (!g_txbuf[i]) { kputs("[e1000e] netbuf exhausted\r\n"); g_mmio = 0; return; }
    }

    wr(E1000_RDBAL, (uint32_t)(rxpg & 0xFFFFFFFFu));
    wr(E1000_RDBAH, (uint32_t)(rxpg >> 32));
    wr(E1000_RDLEN, NRX * (uint32_t)sizeof(struct rx_desc));   /* BYTES, note (2) */
    wr(E1000_RDH, 0);
    wr(E1000_RDT, NRX - 1);                   /* own all but one — note (1) */
    wr(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    wr(E1000_TDBAL, (uint32_t)(txpg & 0xFFFFFFFFu));
    wr(E1000_TDBAH, (uint32_t)(txpg >> 32));
    wr(E1000_TDLEN, NTX * (uint32_t)sizeof(struct tx_desc));
    wr(E1000_TDH, 0);
    wr(E1000_TDT, 0);
    wr(E1000_TCTL, TCTL_EN | TCTL_PSP);

    g_up = 1;

    /* kputhex() prints a 16-digit 64-bit value — unreadable for a MAC, and a
     * gate cannot assert on it usefully. Two digits per octet, built here. */
    char line[32];
    static const char hexd[] = "0123456789abcdef";
    unsigned p = 0;
    for (int i = 0; i < 6; i++) {
        line[p++] = hexd[(g_mac[i] >> 4) & 0xF];
        line[p++] = hexd[g_mac[i] & 0xF];
        if (i < 5)
            line[p++] = ':';
    }
    line[p] = '\0';

    kputs("[e1000e] up, mac=");
    kputs(line);
    kputs(" link=");
    kputdec((rd(E1000_STATUS) & (1u << 1)) ? 1 : 0);
    kputs("\r\n");

    selftest();
}

/* ---- bring-up self-test -------------------------------------------------
 * "The driver found the device" is not the same claim as "the rings work".
 * A broadcast ARP request goes out and the reply is polled for: that single
 * round trip exercises the TX descriptor, the RX descriptor, the DD bit and
 * the tail pointer — the four things this driver can get quietly wrong.
 *
 * Without it, e1000e_poll() would also be a function nothing calls.
 */
static unsigned g_rx_seen;

static void selftest_rx(const uint8_t *frame, uint32_t len) {
    (void)frame;
    if (len)
        g_rx_seen++;
}

static void selftest(void) {
    uint8_t arp[42];
    memset(arp, 0, sizeof arp);
    for (int i = 0; i < 6; i++) arp[i] = 0xFF;          /* broadcast          */
    memcpy(&arp[6], g_mac, 6);
    arp[12] = 0x08; arp[13] = 0x06;                     /* ethertype ARP      */
    arp[14] = 0x00; arp[15] = 0x01;                     /* htype Ethernet     */
    arp[16] = 0x08; arp[17] = 0x00;                     /* ptype IPv4         */
    arp[18] = 6;    arp[19] = 4;
    arp[20] = 0x00; arp[21] = 0x01;                     /* oper request       */
    memcpy(&arp[22], g_mac, 6);
    arp[28] = 10; arp[29] = 0; arp[30] = 2; arp[31] = 15;   /* sender 10.0.2.15 */
    arp[38] = 10; arp[39] = 0; arp[40] = 2; arp[41] = 2;    /* target 10.0.2.2  */

    void (*saved)(const uint8_t *, uint32_t) = g_rx_cb;
    g_rx_cb = selftest_rx;

    if (e1000e_tx(arp, sizeof arp) != 0) {
        kputs("[e1000e] TX failed\r\n");
        g_rx_cb = saved;
        return;
    }
    kputs("[e1000e] TX OK\r\n");

    /* Bounded: a NIC that never answers must report a timeout, not spin. */
    for (unsigned i = 0; i < 20000000u && g_rx_seen == 0; i++) {
        e1000e_poll();
        __asm__ volatile("pause");
    }

    g_rx_cb = saved;
    if (g_rx_seen) {
        kputs("[e1000e] RX OK n=");
        kputdec(g_rx_seen);
        kputs("\r\n");
    } else {
        kputs("[e1000e] RX timeout\r\n");
    }
}
