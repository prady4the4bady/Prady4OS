/* kernel/apic/percpu.c — per-CPU identity area (ADR-030 stage 2, DDR-SMP-2). */
#include "percpu.h"
#include "lapic.h"

static struct percpu g_percpu[PERCPU_MAX];

void percpu_init_cpu(uint32_t cpu_idx) {
    if (cpu_idx >= PERCPU_MAX)
        return;
    g_percpu[cpu_idx].cpu_idx = cpu_idx;
    g_percpu[cpu_idx].apic_id = lapic_id();
    g_percpu[cpu_idx].present = 1;
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
    uint32_t id = lapic_id();
    for (unsigned i = 0; i < PERCPU_MAX; i++)
        if (g_percpu[i].present && g_percpu[i].apic_id == id)
            return &g_percpu[i];
    return 0;
}
