/* kernel/apic/smp.c — SMP bring-up: INIT-SIPI-SIPI, APs parked (ADR-029).
 *
 * The BSP copies the 16-bit trampoline (arch/x86_64/ap_boot.asm) to physical
 * 0x8000, fills its mailbox (kernel CR3, C entry, per-AP stack, cpu index),
 * and sends the MP-init sequence per Intel SDM vol. 3 §8.4: INIT, ~10 ms wait,
 * then two STARTUP IPIs (vector 0x08 -> 0x8000) ~200 µs apart. APs come up one
 * at a time (shared mailbox), announce under a spinlock, mark themselves
 * online, and park (cli/hlt) — no scheduler, no IRQs (ADR-016 stays valid).
 */
#include "smp.h"
#include "lapic.h"
#include "spinlock.h"
#include "vmm.h"
#include "pmm.h"
#include "irq.h"
#include "io.h"
#include "console.h"

#define TRAMP_PHYS    0x8000ull
#define OFF_MB_CR3    0xA0
#define OFF_MB_ENTRY  0xA8
#define OFF_MB_STACK  0xB0
#define OFF_MB_IDX    0xB8

#define IPI_INIT      0x00004500u   /* INIT, assert, fixed dest              */
#define IPI_SIPI      0x00004608u   /* STARTUP, vector 0x08 (page 0x8000)    */

extern const uint8_t ap_tramp_start[], ap_tramp_end[];

static volatile uint32_t g_online = 1;      /* the BSP */
static spinlock_t g_announce_lock = SPINLOCK_INIT;

/* ~N ticks of PIT/APIC time (10 ms each); IF is on, so g_ticks advances. */
static void delay_ticks(uint64_t n) {
    uint64_t until = g_ticks + n;
    while (g_ticks < until)
        __asm__ volatile("pause");
}

/* ~200 µs: port-0x80 writes are the classic ~1 µs I/O delay. */
static void delay_200us(void) {
    for (int i = 0; i < 200; i++)
        io_wait();
}

/* C entry for a freshly long-moded AP (jumped to from the trampoline with its
 * cpu index in RDI and a private stack). Announce, mark online, park. */
void smp_ap_entry(uint32_t idx);
void smp_ap_entry(uint32_t idx) {
    spin_lock(&g_announce_lock);
    kputs("[smp] cpu ");
    kputdec(idx);
    kputs(" up id=");
    kputdec(lapic_apic_id_at(idx));
    kputs("\r\n");
    spin_unlock(&g_announce_lock);
    __atomic_add_fetch(&g_online, 1, __ATOMIC_SEQ_CST);
    for (;;)
        __asm__ volatile("cli; hlt");
}

void smp_start_aps(void) {
    unsigned total = lapic_cpu_count();
    if (total < 2) {
        kputs("[smp] cpus online=1/1\r\n");
        return;
    }

    /* Stage the trampoline at 0x8000 (below the kernel at 0x10000; reserved). */
    uint64_t tlen = (uint64_t)(ap_tramp_end - ap_tramp_start);
    volatile uint8_t *tramp = (volatile uint8_t *)(uintptr_t)TRAMP_PHYS;
    for (uint64_t i = 0; i < tlen; i++)
        tramp[i] = ap_tramp_start[i];

    uint32_t bsp = lapic_id();
    for (unsigned i = 0; i < total; i++) {
        uint32_t id = lapic_apic_id_at(i);
        if (id == bsp)
            continue;
        uint64_t stack = pmm_alloc_pages(2);          /* 16 KiB AP stack */
        if (!stack) {
            kputs("[smp] no AP stack\r\n");
            break;
        }
        *(volatile uint64_t *)(uintptr_t)(TRAMP_PHYS + OFF_MB_CR3)   = vmm_kernel_cr3();
        *(volatile uint64_t *)(uintptr_t)(TRAMP_PHYS + OFF_MB_ENTRY) = (uint64_t)(uintptr_t)smp_ap_entry;
        *(volatile uint64_t *)(uintptr_t)(TRAMP_PHYS + OFF_MB_STACK) = stack + 4 * PAGE_SIZE;
        *(volatile uint32_t *)(uintptr_t)(TRAMP_PHYS + OFF_MB_IDX)   = i;

        uint32_t before = g_online;
        lapic_send_ipi(id, IPI_INIT);
        delay_ticks(2);                               /* >= 10 ms after INIT */
        lapic_send_ipi(id, IPI_SIPI);
        delay_200us();
        if (g_online == before) {                     /* SDM: second SIPI if needed */
            lapic_send_ipi(id, IPI_SIPI);
            delay_200us();
        }
        uint64_t deadline = g_ticks + 100;            /* <= 1 s to come online */
        while (g_online == before && g_ticks < deadline)
            __asm__ volatile("pause");
        if (g_online == before) {
            kputs("[smp] cpu timed out id=");
            kputdec(id);
            kputs("\r\n");
        }
    }

    kputs("[smp] cpus online=");
    kputdec(g_online);
    kputs("/");
    kputdec(total);
    kputs("\r\n");
}
