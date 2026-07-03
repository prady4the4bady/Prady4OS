/* kernel/apic/percpu.c — per-CPU identity area (ADR-030 stages 2+3a).
 *
 * DDR-SMP-3a: this_cpu() reads %gs:0. percpu_init_cpu loads IA32_GS_BASE with
 * this CPU's entry (we are in kernel context when it runs); the SWAPGS
 * discipline in syscall_entry/isr/usermode.asm keeps the kernel base active
 * exactly while CPL 0 code runs. KERNEL_GS_BASE starts 0 = the user's view. */
#include "percpu.h"
#include "lapic.h"

#define MSR_GS_BASE 0xC0000101u

static struct percpu g_percpu[PERCPU_MAX];

void percpu_init_cpu(uint32_t cpu_idx) {
    if (cpu_idx >= PERCPU_MAX)
        return;
    struct percpu *p = &g_percpu[cpu_idx];
    p->self    = p;
    p->cpu_idx = cpu_idx;
    p->apic_id = lapic_id();
    p->present = 1;
    uint64_t base = (uint64_t)(uintptr_t)p;
    __asm__ volatile("wrmsr" :: "c"(MSR_GS_BASE),
                     "a"((uint32_t)base), "d"((uint32_t)(base >> 32)));
}

void percpu_init_bsp(void) {
    uint32_t id = lapic_id();
    for (unsigned i = 0; i < lapic_cpu_count() && i < PERCPU_MAX; i++)
        if (lapic_apic_id_at(i) == id) {
            percpu_init_cpu(i);
            return;
        }
    percpu_init_cpu(0);            /* MADT absent/odd: BSP takes slot 0 */
}

struct percpu *this_cpu(void) {
    struct percpu *p;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(p));   /* self pointer (DDR-SMP-3a) */
    return p;
}
