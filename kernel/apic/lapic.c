/* kernel/apic/lapic.c — Local APIC bring-up + APIC timer (DDR-714 stage A).
 *
 * xAPIC MMIO register layout and semantics per Intel SDM vol. 3 §11 (Advanced
 * Programmable Interrupt Controller): spurious-vector register software-enable
 * (bit 8), LVT timer periodic mode (bit 17), divide-configuration encoding, and
 * EOI at offset 0xB0. The LAPIC page is identity-mapped uncached (VMM_PCD),
 * the same pattern as the PCIe ECAM window; the mapping lands in the shared
 * boot PDPT so every address space sees it (kernel-only leaf).
 */
#include "lapic.h"
#include "acpi.h"
#include "vmm.h"
#include "irq.h"
#include "console.h"

#define MSR_APIC_BASE      0x1Bu
#define APIC_BASE_ENABLE   (1ull << 11)

/* xAPIC MMIO register offsets (Intel SDM vol. 3, table 11-1). */
#define LAPIC_ID           0x020
#define LAPIC_TPR          0x080
#define LAPIC_EOI          0x0B0
#define LAPIC_SVR          0x0F0
#define LAPIC_ICR_LO       0x300   /* write triggers the IPI (bit 12 = busy) */
#define LAPIC_ICR_HI       0x310   /* destination LAPIC id in bits 56..63    */
#define LAPIC_LVT_TIMER    0x320
#define LAPIC_TIMER_ICR    0x380   /* initial count  */
#define LAPIC_TIMER_CCR    0x390   /* current count  */
#define LAPIC_TIMER_DCR    0x3E0   /* divide config  */

#define SVR_ENABLE         0x100u
#define SVR_SPURIOUS_VEC   0xFFu
#define LVT_PERIODIC       0x20000u
#define DCR_DIV16          0x3u

struct madt {
    struct acpi_sdt_header hdr;
    uint32_t lapic_addr;
    uint32_t flags;
    uint8_t  entries[];
} __attribute__((packed));

static volatile uint8_t *g_lapic;   /* identity VA of the LAPIC page, or NULL */
static unsigned g_ncpus;
#define LAPIC_MAX_CPUS 16
static uint8_t g_apic_ids[LAPIC_MAX_CPUS];   /* MADT type-0 LAPIC ids (ADR-029) */

static uint32_t lapic_rd(uint32_t off) {
    return *(volatile uint32_t *)(g_lapic + off);
}
static void lapic_wr(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(g_lapic + off) = val;
}

unsigned lapic_cpu_count(void) { return g_ncpus; }

uint32_t lapic_id(void) {
    return g_lapic ? (lapic_rd(LAPIC_ID) >> 24) : 0;
}

uint32_t lapic_apic_id_at(unsigned i) {
    return (i < g_ncpus) ? g_apic_ids[i] : 0;
}

void lapic_send_ipi(uint32_t dest_id, uint32_t icr_low) {
    if (!g_lapic)
        return;
    lapic_wr(LAPIC_ICR_HI, dest_id << 24);
    lapic_wr(LAPIC_ICR_LO, icr_low);
    while (lapic_rd(LAPIC_ICR_LO) & (1u << 12))   /* delivery-status busy */
        __asm__ volatile("pause");
}

void lapic_eoi(void) {
    lapic_wr(LAPIC_EOI, 0);
}

int lapic_init(void) {
    const struct madt *m = (const struct madt *)acpi_find_table("APIC");
    if (!m) {
        kputs("[apic] absent - PIT retained\r\n");
        return -1;
    }
    uint64_t base = m->lapic_addr;

    /* Count processor-LAPIC entries (type 0) — stage B's CPU roster. */
    const uint8_t *p = m->entries;
    const uint8_t *end = (const uint8_t *)m + m->hdr.length;
    g_ncpus = 0;
    while (p + 2 <= end && p[1] >= 2 && p + p[1] <= end) {
        /* type 0 = processor LAPIC: [2]=ACPI id, [3]=LAPIC id, [4..7]=flags;
         * count only enabled processors (flags bit 0), keep the id (ADR-029). */
        if (p[0] == 0 && p[1] >= 8 && (p[4] & 1) && g_ncpus < LAPIC_MAX_CPUS) {
            g_apic_ids[g_ncpus] = p[3];
            g_ncpus++;
        }
        p += p[1];
    }

    /* The hardware-enable bit in IA32_APIC_BASE is set out of reset; we do not
     * relocate the base. If firmware cleared it, re-enabling requires care we
     * defer — report and fall back to the PIT. */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_APIC_BASE));
    if (!((((uint64_t)hi << 32) | lo) & APIC_BASE_ENABLE)) {
        kputs("[apic] hw-disabled - PIT retained\r\n");
        return -1;
    }

    if (vmm_map(base, base, VMM_RW | VMM_PCD) != 0) {
        kputs("[apic] map failed - PIT retained\r\n");
        return -1;
    }
    g_lapic = (volatile uint8_t *)(uintptr_t)base;

    lapic_wr(LAPIC_SVR, SVR_ENABLE | SVR_SPURIOUS_VEC);   /* software enable */
    lapic_wr(LAPIC_TPR, 0);                               /* accept all classes */

    kputs("[apic] up id=");
    kputdec(lapic_rd(LAPIC_ID) >> 24);
    kputs(" cpus=");
    kputdec(g_ncpus);
    kputs(" base=");
    kputhex(base);
    kputs("\r\n");
    return 0;
}

void lapic_timer_100hz(void) {
    if (!g_lapic)
        return;
    /* Calibrate: free-run the APIC timer (div 16, one-shot, masked LVT is fine —
     * we only read the count-down) across 10 PIT ticks = 100 ms. */
    lapic_wr(LAPIC_TIMER_DCR, DCR_DIV16);
    uint64_t t0 = g_ticks;
    while (g_ticks == t0)                     /* align to a tick edge */
        __asm__ volatile("hlt");
    lapic_wr(LAPIC_TIMER_ICR, 0xFFFFFFFFu);
    uint64_t t1 = g_ticks;
    while (g_ticks < t1 + 10)                 /* 100 ms of PIT time */
        __asm__ volatile("hlt");
    uint32_t elapsed = 0xFFFFFFFFu - lapic_rd(LAPIC_TIMER_CCR);
    uint32_t per_10ms = elapsed / 10;         /* APIC-timer counts per 10 ms */
    if (per_10ms == 0)
        per_10ms = 1;

    /* Periodic 100 Hz on vector 48, then hand the tick over: mask PIT IRQ0. */
    lapic_wr(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | LVT_PERIODIC);
    lapic_wr(LAPIC_TIMER_ICR, per_10ms);
    pic_mask(0);
    kputs("[apic] timer 100Hz (PIT masked) count=");
    kputdec(per_10ms);
    kputs("\r\n");
}
