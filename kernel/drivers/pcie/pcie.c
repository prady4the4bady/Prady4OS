/* kernel/drivers/pcie/pcie.c — PCIe MMCONFIG enumeration (Phase 3). */
#include "pcie.h"
#include "acpi.h"
#include "vmm.h"
#include "pmm.h"          /* PAGE_SIZE */
#include "console.h"

/* MCFG: ACPI table, header + reserved(8) + N allocation entries. */
struct mcfg_alloc {
    uint64_t base;        /* ECAM base for this segment's bus range */
    uint16_t segment;
    uint8_t  start_bus;
    uint8_t  end_bus;
    uint32_t reserved;
} __attribute__((packed));

struct mcfg {
    struct acpi_sdt_header header;
    uint64_t reserved;
    struct mcfg_alloc allocs[];
} __attribute__((packed));

#define ECAM_VBASE   0xFFFFC00000000000ull   /* unused kernel VA window for ECAM */
#define ECAM_BUS_SZ  0x100000ull             /* 1 MiB of config space per bus    */
#define PCIE_REG_MAX 64

static uint64_t g_ecam_phys;
static uint64_t g_ecam_virt;
static uint8_t  g_start_bus;
static struct pcie_device g_devs[PCIE_REG_MAX];
static unsigned g_count;

static const char *class_name(uint8_t cls, uint8_t sub) {
    switch (cls) {
    case 0x01: return (sub == 0x06) ? "SATA controller"
                    : (sub == 0x08) ? "NVMe controller" : "storage controller";
    case 0x02: return "network controller";
    case 0x03: return "display controller";
    case 0x06: return (sub == 0x00) ? "host bridge"
                    : (sub == 0x01) ? "ISA bridge" : "bridge";
    case 0x0C: return "serial bus controller";
    default:   return "device";
    }
}

uint32_t pcie_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off) {
    uint64_t a = g_ecam_virt + ((uint64_t)(bus - g_start_bus) << 20)
               + ((uint64_t)dev << 15) + ((uint64_t)func << 12) + off;
    return *(volatile uint32_t *)(uintptr_t)a;
}

void pcie_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off, uint32_t val) {
    uint64_t a = g_ecam_virt + ((uint64_t)(bus - g_start_bus) << 20)
               + ((uint64_t)dev << 15) + ((uint64_t)func << 12) + off;
    *(volatile uint32_t *)(uintptr_t)a = val;
}

unsigned pcie_device_count(void) { return g_count; }
const struct pcie_device *pcie_device_get(unsigned i) {
    return (i < g_count) ? &g_devs[i] : 0;
}

static void record(uint8_t bus, uint8_t dev, uint8_t func, uint32_t id) {
    uint32_t cr = pcie_read32(bus, dev, func, 0x08);   /* rev|prog_if|subclass|class */
    struct pcie_device d;
    d.bus = bus; d.dev = dev; d.func = func;
    d.vendor_id = id & 0xFFFF;
    d.device_id = id >> 16;
    d.revision  = cr & 0xFF;
    d.prog_if   = (cr >> 8) & 0xFF;
    d.subclass  = (cr >> 16) & 0xFF;
    d.class_code = (cr >> 24) & 0xFF;
    if (g_count < PCIE_REG_MAX)
        g_devs[g_count++] = d;

    kputs("  ");
    kputdec(bus); kputs(":"); kputdec(dev); kputs("."); kputdec(func);
    kputs("  "); kputhex(d.vendor_id); kputs(":"); kputhex(d.device_id);
    kputs("  class "); kputhex(d.class_code); kputs("/"); kputhex(d.subclass);
    kputs("  "); kputs(class_name(d.class_code, d.subclass));
    kputs("\r\n");
}

static void scan_bus(uint8_t bus) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id0 = pcie_read32(bus, dev, 0, 0);
        if ((id0 & 0xFFFF) == 0xFFFF)
            continue;                          /* no device in this slot */
        record(bus, dev, 0, id0);
        uint32_t htype = pcie_read32(bus, dev, 0, 0x0C);
        int multifunc = (htype >> 16) & 0x80;
        if (!multifunc)
            continue;
        for (uint8_t func = 1; func < 8; func++) {
            uint32_t id = pcie_read32(bus, dev, func, 0);
            if ((id & 0xFFFF) != 0xFFFF)
                record(bus, dev, func, id);
        }
    }
}

void pcie_init(void) {
    const struct acpi_sdt_header *h = acpi_find_table("MCFG");
    if (!h) {
        kputs("PCIe: no MCFG table (need a PCIe machine, e.g. QEMU q35)\r\n");
        return;
    }
    const struct mcfg *m = (const struct mcfg *)h;
    unsigned n = (m->header.length - (uint32_t)sizeof(struct acpi_sdt_header) - 8u)
                 / (unsigned)sizeof(struct mcfg_alloc);
    if (n == 0) {
        kputs("PCIe: MCFG has no allocations\r\n");
        return;
    }

    g_ecam_phys = m->allocs[0].base;
    g_start_bus = m->allocs[0].start_bus;
    g_ecam_virt = ECAM_VBASE;

    kputs("PCIe: ECAM ");
    kputhex(g_ecam_phys);
    kputs(" (bus ");
    kputdec(g_start_bus);
    kputs("+) — scanning bus 0\r\n");

    /* Map bus 0's 1 MiB of config space, uncached (MMIO). */
    for (uint64_t off = 0; off < ECAM_BUS_SZ; off += PAGE_SIZE)
        vmm_map(g_ecam_virt + off, g_ecam_phys + off, VMM_RW | VMM_PCD);

    g_count = 0;
    scan_bus(g_start_bus);

    kputs("PCIe: ");
    kputdec(g_count);
    kputs(" devices enumerated\r\n");
}

/* ---- MSI-X capability helpers (DDR-774a) --------------------------------- */
/* Extracted verbatim from virtio_pci_msix_setup() so NVMe (DDR-774b) can reuse
 * them. Capability offsets are DWORD-aligned by the PCI spec, so the byte fields
 * (ID at +0, next at +1, message control at +2) come out of one pcie_read32. */

uint8_t pcie_msix_find(uint8_t bus, uint8_t dev, uint8_t func,
                       uint8_t *bir, uint32_t *table_off) {
    uint8_t cap = (uint8_t)(pcie_read32(bus, dev, func, 0x34) & 0xFCu);
    while (cap) {
        uint32_t hdr = pcie_read32(bus, dev, func, cap);
        if ((hdr & 0xFFu) == 0x11u) {                 /* MSI-X capability ID */
            uint32_t tab = pcie_read32(bus, dev, func, (uint16_t)(cap + 4));
            if (bir)       *bir = (uint8_t)(tab & 7u);        /* BAR index    */
            if (table_off) *table_off = tab & ~7u;            /* byte offset  */
            return cap;
        }
        cap = (uint8_t)((hdr >> 8) & 0xFCu);          /* next capability ptr */
    }
    return 0;
}

void pcie_msix_program(uint8_t bus, uint8_t dev, uint8_t func, uint8_t cap,
                       volatile uint32_t *entry0, uint8_t vector, uint32_t apic_id) {
    /* Enable MSI-X (message control bit 15 = bit 31 of the capability dword). */
    uint32_t mc = pcie_read32(bus, dev, func, cap);
    pcie_write32(bus, dev, func, cap, mc | (1u << 31));

    entry0[0] = 0xFEE00000u | (apic_id << 12);   /* message address: this LAPIC */
    entry0[1] = 0;                               /* address high                */
    entry0[2] = vector;                          /* data: fixed, edge, vector   */
    entry0[3] = 0;                               /* vector control: unmasked    */
}

void pcie_intx_disable(uint8_t bus, uint8_t dev, uint8_t func) {
    uint32_t cmd = pcie_read32(bus, dev, func, 0x04);
    pcie_write32(bus, dev, func, 0x04, cmd | (1u << 10));
}
