/* kernel/drivers/virtio/virtio_pci.c — modern virtio 1.0 PCIe transport. */
#include "virtio_pci.h"
#include "pcie.h"
#include "vmm.h"
#include "pmm.h"
#include "console.h"

/* virtio PCI capability cfg_type values */
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define PCI_CAP_ID_VNDR 0x09

struct virtio_pci_common_cfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} __attribute__((packed));

#define BAR_VBASE 0xFFFFD00000000000ull    /* base of the per-BAR mapping windows  */
#define BAR_MAP_SZ 0x4000u                 /* 16 KiB per BAR (covers virtio caps)  */

/* Monotonic allocator of 64 KiB VA windows — each mapped BAR (across all virtio
 * devices) gets a distinct window so devices never share a virtual mapping. */
static uint64_t g_next_bar_va = BAR_VBASE;

static uint8_t  cfg8 (struct virtio_pci_dev *d, uint16_t o) {
    return (pcie_read32(d->bus, d->dev, d->func, o & ~3u) >> ((o & 3) * 8)) & 0xFF;
}
static uint32_t cfg32(struct virtio_pci_dev *d, uint16_t o) {
    return pcie_read32(d->bus, d->dev, d->func, o);
}

/* Map (once per device) the given BAR's MMIO region, uncached; return its VA. */
static volatile uint8_t *map_bar(struct virtio_pci_dev *d, uint8_t bar) {
    if (bar >= 6)
        return 0;
    if (d->bar_virt[bar])
        return d->bar_virt[bar];

    uint32_t lo = cfg32(d, 0x10 + bar * 4);
    uint64_t base;
    if (((lo >> 1) & 3) == 2) {            /* 64-bit memory BAR */
        uint32_t hi = cfg32(d, 0x10 + (bar + 1) * 4);
        base = (uint64_t)(lo & 0xFFFFFFF0u) | ((uint64_t)hi << 32);
    } else {
        base = lo & 0xFFFFFFF0u;
    }
    uint64_t va = g_next_bar_va;           /* unique window per BAR, per device */
    g_next_bar_va += 0x10000;
    for (uint32_t off = 0; off < BAR_MAP_SZ; off += PAGE_SIZE)
        vmm_map(va + off, base + off, VMM_RW | VMM_PCD);
    d->bar_virt[bar] = (volatile uint8_t *)(uintptr_t)va;
    return d->bar_virt[bar];
}

int virtio_pci_attach(struct virtio_pci_dev *d, uint8_t bus, uint8_t dev, uint8_t func) {
    d->bus = bus; d->dev = dev; d->func = func;
    d->common = d->notify = d->isr = d->devcfg = 0;
    d->notify_mult = 0;
    d->irq = cfg8(d, 0x3C);
    for (int i = 0; i < 6; i++)
        d->bar_virt[i] = 0;

    /* Command register: enable memory space + bus master (DMA), keep INTx on. */
    uint32_t cmd = cfg32(d, 0x04);
    cmd |= (1u << 1) | (1u << 2);          /* memory space, bus master */
    cmd &= ~(1u << 10);                     /* clear INTx-disable */
    pcie_write32(d->bus, d->dev, d->func, 0x04, cmd);

    /* capabilities present? (status bit 4) */
    if (!((cfg32(d, 0x04) >> 16) & (1 << 4)))
        return -1;

    uint8_t cap = cfg8(d, 0x34) & 0xFC;
    while (cap) {
        uint8_t id   = cfg8(d, cap + 0);
        uint8_t next = cfg8(d, cap + 1);
        if (id == PCI_CAP_ID_VNDR) {
            uint8_t  type   = cfg8(d, cap + 3);
            uint8_t  bar    = cfg8(d, cap + 4);
            uint32_t offset = cfg32(d, cap + 8);
            volatile uint8_t *bv = map_bar(d, bar);
            if (bv) {
                volatile uint8_t *p = bv + offset;
                switch (type) {
                case VIRTIO_PCI_CAP_COMMON_CFG: d->common = p; break;
                case VIRTIO_PCI_CAP_NOTIFY_CFG:
                    d->notify = p;
                    d->notify_mult = cfg32(d, cap + 16);
                    break;
                case VIRTIO_PCI_CAP_ISR_CFG:    d->isr = p; break;
                case VIRTIO_PCI_CAP_DEVICE_CFG: d->devcfg = p; break;
                default: break;
                }
            }
        }
        cap = next & 0xFC;
    }
    return (d->common && d->notify && d->isr) ? 0 : -1;
}

uint64_t virtio_pci_negotiate(struct virtio_pci_dev *d, uint64_t wanted) {
    volatile struct virtio_pci_common_cfg *c =
        (volatile struct virtio_pci_common_cfg *)d->common;

    c->device_status = 0;                  /* reset */
    while (c->device_status != 0) { }
    c->device_status = VIRTIO_STATUS_ACK;
    c->device_status = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER;

    c->device_feature_select = 0;
    uint64_t feat = c->device_feature;
    c->device_feature_select = 1;
    feat |= (uint64_t)c->device_feature << 32;

    uint64_t accept = feat & wanted;
    c->driver_feature_select = 0;
    c->driver_feature = (uint32_t)accept;
    c->driver_feature_select = 1;
    c->driver_feature = (uint32_t)(accept >> 32);

    c->device_status = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK;
    if (!(c->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        c->device_status = VIRTIO_STATUS_FAILED;
        return 0;
    }
    return accept;
}

int virtio_pci_setup_queue(struct virtio_pci_dev *d, struct virtq *vq, uint16_t qidx) {
    volatile struct virtio_pci_common_cfg *c =
        (volatile struct virtio_pci_common_cfg *)d->common;

    c->queue_select = qidx;
    uint16_t size = c->queue_size;
    if (size == 0)
        return -1;
    if (size > 256)
        size = 256;                        /* one page per ring */
    if (virtq_alloc(vq, size) != 0)
        return -1;

    c->queue_size   = size;
    c->queue_desc   = (uint64_t)(uintptr_t)vq->desc;
    c->queue_driver = (uint64_t)(uintptr_t)vq->avail;
    c->queue_device = (uint64_t)(uintptr_t)vq->used;
    vq->notify_off  = c->queue_notify_off;
    c->queue_enable = 1;
    return 0;
}

void virtio_pci_driver_ok(struct virtio_pci_dev *d) {
    volatile struct virtio_pci_common_cfg *c =
        (volatile struct virtio_pci_common_cfg *)d->common;
    c->device_status = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                       VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;
}

void virtio_pci_notify(struct virtio_pci_dev *d, struct virtq *vq, uint16_t qidx) {
    virtio_mb();
    volatile uint16_t *n =
        (volatile uint16_t *)(d->notify + (uint64_t)vq->notify_off * d->notify_mult);
    *n = qidx;
}

uint8_t virtio_pci_isr_ack(struct virtio_pci_dev *d) {
    return *d->isr;                        /* read-to-clear */
}

/* DDR-714C1: point queue 0's completion at MSI-X table entry 0, delivering
 * `vector` to LAPIC `apic_id` — bypassing the shared INTx line and the 8259
 * entirely. Returns -1 (caller falls back to INTx) if the device has no MSI-X
 * capability or rejects the vector mapping. Call AFTER virtio_pci_setup_queue
 * (queue 0 selected), BEFORE driver_ok. */
int virtio_pci_msix_setup(struct virtio_pci_dev *d, uint8_t vector, uint32_t apic_id,
                          uint16_t nqueues) {
    /* Find the MSI-X capability (ID 0x11). */
    uint8_t cap = cfg8(d, 0x34) & 0xFC;
    while (cap && cfg8(d, cap + 0) != 0x11)
        cap = cfg8(d, cap + 1) & 0xFC;
    if (!cap)
        return -1;

    uint32_t tab = cfg32(d, cap + 4);      /* table offset (bits 31..3) + BIR (2..0) */
    volatile uint8_t *bv = map_bar(d, (uint8_t)(tab & 7u));
    if (!bv)
        return -1;
    volatile uint32_t *e = (volatile uint32_t *)(bv + (tab & ~7u));   /* entry 0 */

    /* Enable MSI-X (message control bit 15), function-mask off (bit 14), while
     * masked entries are programmed; then unmask entry 0. */
    uint32_t mc = cfg32(d, cap);           /* [31:16] = message control */
    pcie_write32(d->bus, d->dev, d->func, cap, mc | (1u << 31));      /* MSI-X enable */
    e[0] = 0xFEE00000u | (apic_id << 12);  /* message address: this LAPIC        */
    e[1] = 0;                              /* address high                        */
    e[2] = vector;                         /* message data: fixed, edge, vector   */
    e[3] = 0;                              /* vector control: unmasked            */

    /* Route queues 0..nqueues-1 ALL to table entry 0 (one vector per device —
     * C2: net RX+TX, input event+status); 0xFFFF read-back = rejected. */
    volatile struct virtio_pci_common_cfg *c =
        (volatile struct virtio_pci_common_cfg *)d->common;
    c->msix_config = 0xFFFF;               /* no config-change vector */
    for (uint16_t q = 0; q < nqueues; q++) {
        c->queue_select = q;
        c->queue_msix_vector = 0;
        if (c->queue_msix_vector == 0xFFFF)
            return -1;
    }

    /* This function signals via MSI-X now — disable its INTx assertions. */
    uint32_t cmd = cfg32(d, 0x04);
    pcie_write32(d->bus, d->dev, d->func, 0x04, cmd | (1u << 10));
    return 0;
}
