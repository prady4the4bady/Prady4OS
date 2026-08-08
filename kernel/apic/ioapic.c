/* kernel/apic/ioapic.c — I/O APIC + q35 GSI routing (Group 4 item 28, DDR-874).
 *
 * WHAT THIS REPLACES. The 8259 PIC delivers every legacy IRQ to the BSP, full
 * stop. The I/O APIC has a redirection table instead: one entry per Global
 * System Interrupt, each naming a vector AND a destination LAPIC. That is the
 * mechanism per-CPU interrupt affinity needs; without it "route this device to
 * that core" has nowhere to be expressed.
 *
 * THE PART THAT IS EASY TO GET WRONG — INTERRUPT SOURCE OVERRIDES.
 * ISA IRQ n does NOT reliably equal GSI n. On q35 the PIT is the standard
 * example: firmware routes IRQ 0 to GSI 2, and the MADT says so with an
 * Interrupt Source Override entry. A driver that programs redirection entry 0
 * for the timer on such a board arms an input nothing is wired to, gets no
 * interrupts, and looks exactly like a dead timer. So the override table is
 * parsed first and every route goes through it — there is no path in this file
 * that uses a raw IRQ number as a GSI.
 *
 * MASKED-BY-DEFAULT. Every entry is masked at init and unmasked only by an
 * explicit route. The alternative — inheriting whatever the firmware left
 * armed — means taking interrupts for devices no driver has claimed, with no
 * handler installed.
 */
#include "ioapic.h"
#include "acpi.h"
#include "console.h"
#include "vmm.h"
#include "string.h"

#define IOAPIC_REGSEL   0x00        /* MMIO window: register selector */
#define IOAPIC_IOWIN    0x10        /* MMIO window: data              */

#define IOAPIC_REG_ID   0x00
#define IOAPIC_REG_VER  0x01
#define IOAPIC_REG_RED  0x10        /* redirection table base, 2 dwords each */

#define REDIR_MASKED    (1u << 16)

/* MADT entry types we care about. */
#define MADT_IOAPIC     1
#define MADT_ISO        2

#define MAX_ISO 16

struct madt_iso {
    uint8_t  bus;
    uint8_t  source;                /* legacy ISA IRQ */
    uint32_t gsi;
    uint16_t flags;
};

static volatile uint8_t *g_base;
static uint32_t g_gsi_base;
static unsigned g_redirs;
static struct madt_iso g_iso[MAX_ISO];
static unsigned g_iso_count;
static int g_ok;

static uint32_t ioapic_read(uint8_t reg) {
    *(volatile uint32_t *)(g_base + IOAPIC_REGSEL) = reg;
    return *(volatile uint32_t *)(g_base + IOAPIC_IOWIN);
}

static void ioapic_write(uint8_t reg, uint32_t val) {
    *(volatile uint32_t *)(g_base + IOAPIC_REGSEL) = reg;
    *(volatile uint32_t *)(g_base + IOAPIC_IOWIN) = val;
}

uint32_t ioapic_gsi_for_irq(uint8_t irq) {
    for (unsigned i = 0; i < g_iso_count; i++)
        if (g_iso[i].source == irq)
            return g_iso[i].gsi;
    return (uint32_t)irq;           /* identity when the MADT says nothing */
}

unsigned ioapic_redir_count(void) { return g_redirs; }
int      ioapic_available(void)   { return g_ok; }

int ioapic_init(void) {
    const struct acpi_sdt_header *madt = acpi_find_table("APIC");
    if (!madt) {
        kputs("[ioapic] no MADT — staying on the 8259 PIC\r\n");
        return 0;
    }

    /* MADT layout: a 36-byte header (SDT + local APIC address + flags), then a
     * run of variable-length entries, each {type, length, ...}. The length
     * field is what advances the cursor; trusting a per-type size would walk
     * off the end the moment firmware emits a longer revision of an entry. */
    const uint8_t *p   = (const uint8_t *)madt + 44;
    const uint8_t *end = (const uint8_t *)madt + madt->length;

    while (p + 2 <= end) {
        uint8_t type = p[0], len = p[1];
        if (len < 2 || p + len > end)
            break;                  /* malformed: stop rather than guess */

        if (type == MADT_IOAPIC && len >= 12 && !g_base) {
            uint32_t addr;
            uint32_t gsib;
            memcpy(&addr, p + 4, 4);
            memcpy(&gsib, p + 8, 4);
            g_base     = (volatile uint8_t *)(uintptr_t)addr;
            g_gsi_base = gsib;
        } else if (type == MADT_ISO && len >= 10 && g_iso_count < MAX_ISO) {
            struct madt_iso *e = &g_iso[g_iso_count++];
            e->bus    = p[2];
            e->source = p[3];
            memcpy(&e->gsi, p + 4, 4);
            memcpy(&e->flags, p + 8, 2);
        }
        p += len;
    }

    if (!g_base) {
        kputs("[ioapic] MADT has no I/O APIC — staying on the 8259 PIC\r\n");
        return 0;
    }

    /* The MADT gives a PHYSICAL address, and 0xFEC00000 is high MMIO that the
     * boot identity map does not cover — the ACPI tables happen to live in
     * identity-mapped low RAM, which makes it easy to assume the same of this.
     * It is not: touching it unmapped faults in ioapic_init, which is exactly
     * how this first presented. Map it uncached, as lapic.c does for 0xFEE00000
     * (VMM_PCD: an MMIO register file must never be cached, or a write can sit
     * in a cache line instead of reaching the chip). */
    if (vmm_map((uint64_t)(uintptr_t)g_base, (uint64_t)(uintptr_t)g_base,
                VMM_RW | VMM_PCD) != 0) {
        kputs("[ioapic] cannot map MMIO - staying on the 8259 PIC\r\n");
        g_base = 0;
        return 0;
    }

    g_redirs = ((ioapic_read(IOAPIC_REG_VER) >> 16) & 0xFF) + 1;

    /* Mask everything before anything can fire. */
    for (unsigned i = 0; i < g_redirs; i++) {
        ioapic_write((uint8_t)(IOAPIC_REG_RED + i * 2), REDIR_MASKED);
        ioapic_write((uint8_t)(IOAPIC_REG_RED + i * 2 + 1), 0);
    }

    g_ok = 1;
    kputs("[ioapic] gsi_base=");
    kputdec(g_gsi_base);
    kputs(" redirs=");
    kputdec(g_redirs);
    kputs(" overrides=");
    kputdec(g_iso_count);
    kputs("\r\n");
    return 1;
}

int ioapic_route(uint8_t irq, uint8_t vector, uint8_t apic_id) {
    if (!g_ok)
        return -1;

    uint32_t gsi = ioapic_gsi_for_irq(irq);
    if (gsi < g_gsi_base)
        return -1;
    uint32_t idx = gsi - g_gsi_base;
    if (idx >= g_redirs)
        return -1;

    /* Polarity and trigger mode come from the override flags when present.
     * ISA defaults are active-high, edge-triggered; a PCI line arriving via an
     * override is typically active-low level-triggered, and programming the ISA
     * default for it would either miss the edge or never de-assert. */
    uint32_t low = vector;
    for (unsigned i = 0; i < g_iso_count; i++) {
        if (g_iso[i].source != irq)
            continue;
        uint16_t f = g_iso[i].flags;
        if ((f & 0x3) == 0x3) low |= (1u << 13);        /* active low        */
        if (((f >> 2) & 0x3) == 0x3) low |= (1u << 15); /* level triggered   */
        break;
    }

    ioapic_write((uint8_t)(IOAPIC_REG_RED + idx * 2 + 1),
                 (uint32_t)apic_id << 24);              /* destination LAPIC */
    ioapic_write((uint8_t)(IOAPIC_REG_RED + idx * 2), low);  /* unmasked      */
    return 0;
}

static void set_mask(uint8_t irq, int masked) {
    if (!g_ok)
        return;
    uint32_t gsi = ioapic_gsi_for_irq(irq);
    if (gsi < g_gsi_base)
        return;
    uint32_t idx = gsi - g_gsi_base;
    if (idx >= g_redirs)
        return;
    uint32_t low = ioapic_read((uint8_t)(IOAPIC_REG_RED + idx * 2));
    low = masked ? (low | REDIR_MASKED) : (low & ~REDIR_MASKED);
    ioapic_write((uint8_t)(IOAPIC_REG_RED + idx * 2), low);
}

void ioapic_mask(uint8_t irq)   { set_mask(irq, 1); }
void ioapic_unmask(uint8_t irq) { set_mask(irq, 0); }
