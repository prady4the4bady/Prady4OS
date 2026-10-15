/* kernel/drivers/usb/xhci.c — xHCI controller bring-up and port detection.
 *
 * DDR-1161, USB piece 1. Finds one xHCI controller (PCI 0C/03/30), maps BAR0,
 * reads the capability registers, halts and resets the controller with bounded
 * waits, and reports which root-hub ports have a device connected. No rings, no
 * slots, no enumeration: nothing here makes a USB device usable yet.
 */
#include "xhci.h"
#include "console.h"
#include "pcie.h"
#include "vmm.h"
#include "pmm.h"

#define XHCI_BAR_VBASE 0xFFFFD60000000000ull   /* clear of ECAM/virtio/NVMe/GOP */
#define XHCI_BAR_MAP   0x10000u                 /* 64 KiB covers QEMU's BAR0 */

/* Capability registers (offsets from BAR0). */
#define CAP_CAPLENGTH  0x00                     /* byte */
#define CAP_HCSPARAMS1 0x04
/* Operational registers (offsets from BAR0 + CAPLENGTH). */
#define OP_USBCMD      0x00
#define OP_USBSTS      0x04
#define OP_PORTSC(n)   (0x400u + 0x10u * ((n) - 1u))   /* n is 1-based */

#define USBCMD_RS      (1u << 0)
#define USBCMD_HCRST   (1u << 1)
#define USBSTS_HCH     (1u << 0)
#define USBSTS_CNR     (1u << 11)
#define PORTSC_CCS     (1u << 0)

static int g_xhci_up;

static inline uint32_t rd32(volatile uint8_t *b, uint32_t off) {
    return *(volatile uint32_t *)(b + off);
}
static inline void wr32(volatile uint8_t *b, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(b + off) = v;
}

/* Bounded wait for (reg & mask) == want. 0 on success, -1 on timeout. */
static int wait_reg(volatile uint8_t *b, uint32_t off, uint32_t mask, uint32_t want) {
    for (uint64_t i = 0; i < 50000000ull; i++) {
        if ((rd32(b, off) & mask) == want)
            return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

void xhci_init(uint8_t bus, uint8_t dev, uint8_t func) {
    if (g_xhci_up)
        return;                                 /* one controller this piece */

    uint32_t cmd = pcie_read32(bus, dev, func, 0x04);
    cmd |= (1u << 1) | (1u << 2);               /* memory space + bus master */
    pcie_write32(bus, dev, func, 0x04, cmd);

    uint32_t lo = pcie_read32(bus, dev, func, 0x10);
    uint64_t base = lo & 0xFFFFFFF0u;
    if (((lo >> 1) & 3) == 2)                   /* 64-bit memory BAR */
        base |= (uint64_t)pcie_read32(bus, dev, func, 0x14) << 32;
    if (!base) {
        kputs("[xhci] FAIL bar\r\n");
        return;
    }
    for (uint32_t off = 0; off < XHCI_BAR_MAP; off += PAGE_SIZE)
        vmm_map(XHCI_BAR_VBASE + off, base + off, VMM_RW | VMM_PCD);
    volatile uint8_t *cap = (volatile uint8_t *)XHCI_BAR_VBASE;

    /* CAPLENGTH and HCIVERSION share dword 0. Read it as a dword: QEMU's
     * capability region returns 0 for a 16-bit read at offset 2 (measured). */
    uint32_t cap0   = rd32(cap, CAP_CAPLENGTH);
    uint8_t  caplen = (uint8_t)(cap0 & 0xFF);
    uint32_t hciver = cap0 >> 16;
    uint32_t hcs1   = rd32(cap, CAP_HCSPARAMS1);
    uint32_t slots  = hcs1 & 0xFF;
    uint32_t ports  = (hcs1 >> 24) & 0xFF;
    volatile uint8_t *op = cap + caplen;

    /* Halt, then reset. The spec requires HCH=1 before HCRST is set. */
    wr32(op, OP_USBCMD, rd32(op, OP_USBCMD) & ~USBCMD_RS);
    if (wait_reg(op, OP_USBSTS, USBSTS_HCH, USBSTS_HCH) < 0) {
        kputs("[xhci] FAIL halt\r\n");
        return;
    }
    wr32(op, OP_USBCMD, USBCMD_HCRST);
    if (wait_reg(op, OP_USBCMD, USBCMD_HCRST, 0) < 0 ||
        wait_reg(op, OP_USBSTS, USBSTS_CNR, 0) < 0) {
        kputs("[xhci] FAIL reset\r\n");
        return;
    }
    g_xhci_up = 1;

    kputs("[xhci] up hciver="); kputhex(hciver);
    kputs(" slots=");           kputdec(slots);
    kputs(" ports=");           kputdec(ports);
    kputs("\r\n");

    unsigned connected = 0;
    for (uint32_t p = 1; p <= ports && p <= 255; p++) {
        uint32_t sc = rd32(op, OP_PORTSC(p));
        if (!(sc & PORTSC_CCS))
            continue;
        connected++;
        kputs("[xhci] port="); kputdec(p);
        kputs(" ccs=1 speed="); kputdec((sc >> 10) & 0xF);
        kputs("\r\n");
    }
    kputs("[xhci] connected="); kputdec(connected); kputs("\r\n");
}
