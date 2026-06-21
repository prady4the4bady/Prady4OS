/* kernel/drivers/net/virtio_net.c — virtio-net over the virtio transport (NET-A).
 *
 * Reuses the modern virtio-pci transport (no re-implementation of negotiation or
 * ring management). Sets up the RX (queue 0) and TX (queue 1) virtqueues, reads
 * the MAC from device config, arms RX buffers, and verifies the data path by
 * transmitting one Ethernet frame and reaping its TX completion off the used
 * ring (which QEMU posts regardless of where the packet goes, so this is a
 * reliable functional check). Raw Ethernet only — a socket API and true peer
 * loopback (which needs a tap/socket netdev rather than QEMU's SLIRP "user" net)
 * are deferred to NET-B / later.
 */
#include "virtio_net.h"
#include "netbuf.h"
#include "virtio.h"
#include "virtio_pci.h"
#include "console.h"
#include "irq.h"
#include "string.h"

extern void irq_register(unsigned irq, void (*fn)(void));   /* kernel/idt.c */

#define VIRTIO_NET_F_MAC  (1ull << 5)
#define VNET_HDR_LEN      12u            /* virtio_net_hdr with VIRTIO_F_VERSION_1 */
#define VNET_RX_BUFS      16
#define VNET_TX_SPIN      200000000u     /* bounded wait for TX completion */

struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} __attribute__((packed));
_Static_assert(sizeof(struct virtio_net_hdr) == 12, "virtio_net_hdr (v1) must be 12 bytes");

static struct virtio_pci_dev g_dev;
static struct virtq g_rx, g_tx;
static uint8_t  g_mac[6];
static volatile int g_tx_done;
static volatile int g_rx_seen;
static int g_up;

static void put_hex2(uint8_t b) {
    static const char hx[] = "0123456789abcdef";
    kputc(hx[(b >> 4) & 0xF]);
    kputc(hx[b & 0xF]);
}

/* Shared INTx handler: ack the ISR, then reap completed TX and RX chains. */
static void net_irq(void) {
    uint8_t isr = virtio_pci_isr_ack(&g_dev);
    if (!(isr & 1))
        return;
    uint32_t len;
    int head;
    while ((head = virtq_pop_used(&g_tx, &len)) >= 0) {
        virtq_free_chain(&g_tx, head);
        g_tx_done = 1;
    }
    while ((head = virtq_pop_used(&g_rx, &len)) >= 0) {
        virtq_free_chain(&g_rx, head);     /* baseline: note arrival, no re-arm/parse */
        g_rx_seen = 1;
    }
}

static void net_arm_rx(void) {
    for (int i = 0; i < VNET_RX_BUFS; i++) {
        uint64_t b = netbuf_alloc();
        if (!b)
            break;
        struct virtq_buf rb = { b, NETBUF_SIZE, 1 };   /* device writes incoming frames */
        int h = virtq_add(&g_rx, &rb, 1);
        if (h < 0) { netbuf_free(b); break; }
        virtq_publish(&g_rx, h);
    }
}

/* Transmit one broadcast frame and wait for the device to return its descriptor
 * on the used ring. Returns 0 on TX completion, -1 on timeout/alloc failure. */
static int net_tx_test(void) {
    uint64_t b = netbuf_alloc();
    if (!b)
        return -1;
    struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)(uintptr_t)b;
    memset(hdr, 0, sizeof *hdr);                    /* no checksum offload / GSO */
    uint8_t *eth = (uint8_t *)(uintptr_t)(b + VNET_HDR_LEN);
    for (int i = 0; i < 6; i++) eth[i] = 0xFF;      /* dst: broadcast */
    for (int i = 0; i < 6; i++) eth[6 + i] = g_mac[i];
    eth[12] = 0x08; eth[13] = 0x06;                 /* ethertype: ARP */
    memset(eth + 14, 0, 28);                        /* placeholder ARP body */
    uint32_t flen = VNET_HDR_LEN + 14 + 28;

    struct virtq_buf tb = { b, flen, 0 };           /* driver writes; device reads */
    g_tx_done = 0;
    int h = virtq_add(&g_tx, &tb, 1);
    if (h < 0) { netbuf_free(b); return -1; }
    virtq_publish(&g_tx, h);
    virtio_pci_notify(&g_dev, &g_tx, 1);
    /* Pre-scheduler context: spin with interrupts enabled so net_irq can run. */
    for (volatile uint32_t s = 0; s < VNET_TX_SPIN && !g_tx_done; s++)
        ;
    return g_tx_done ? 0 : -1;
}

void virtio_net_init(uint8_t bus, uint8_t dev, uint8_t func) {
    if (g_up)
        return;                                     /* single instance (baseline) */
    if (virtio_pci_attach(&g_dev, bus, dev, func) != 0) {
        kputs("virtio-net: attach failed\r\n");
        return;
    }
    uint64_t got = virtio_pci_negotiate(&g_dev, VIRTIO_F_VERSION_1 | VIRTIO_NET_F_MAC);
    if (!(got & VIRTIO_F_VERSION_1)) {
        kputs("virtio-net: modern negotiation failed\r\n");
        return;
    }
    if (virtio_pci_setup_queue(&g_dev, &g_rx, 0) != 0 ||
        virtio_pci_setup_queue(&g_dev, &g_tx, 1) != 0) {
        kputs("virtio-net: queue setup failed\r\n");
        return;
    }
    if (got & VIRTIO_NET_F_MAC)
        for (int i = 0; i < 6; i++)
            g_mac[i] = g_dev.devcfg[i];

    netbuf_init();
    net_arm_rx();

    irq_register(g_dev.irq, net_irq);
    pic_unmask(g_dev.irq);
    virtio_pci_driver_ok(&g_dev);
    virtio_pci_notify(&g_dev, &g_rx, 0);            /* advertise RX availability */
    g_up = 1;

    kputs("[net] virtio-net up MAC=");
    for (int i = 0; i < 6; i++) { if (i) kputc(':'); put_hex2(g_mac[i]); }
    kputs(" IRQ ");
    kputdec(g_dev.irq);
    kputs("\r\n");

    kputs(net_tx_test() == 0 ? "[net] virtio-net TX OK\r\n"
                             : "[net] virtio-net TX timeout\r\n");
}
