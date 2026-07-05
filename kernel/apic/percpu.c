/* kernel/apic/percpu.c — per-CPU identity area (ADR-030 stages 2+3a).
 *
 * DDR-SMP-3a: this_cpu() reads %gs:0. percpu_init_cpu loads IA32_GS_BASE with
 * this CPU's entry (we are in kernel context when it runs); the SWAPGS
 * discipline in syscall_entry/isr/usermode.asm keeps the kernel base active
 * exactly while CPL 0 code runs. KERNEL_GS_BASE starts 0 = the user's view. */
#include "percpu.h"
#include "lapic.h"
#include "tss.h"

#define MSR_GS_BASE 0xC0000101u

static struct percpu g_percpu[PERCPU_MAX];

/* Fixed offsets consumed by syscall_entry.asm ([gs:16]) — keep in lockstep. */
_Static_assert(__builtin_offsetof(struct percpu, current) == 8,
               "percpu.current must be at offset 8");
_Static_assert(__builtin_offsetof(struct percpu, kstack_top) == 16,
               "percpu.kstack_top must be at offset 16 (syscall_entry.asm)");
/* cap-4: the SYSCALL-entry snapshot block, written by syscall_entry.asm at
 * these gs-relative offsets — keep in lockstep with the asm. */
_Static_assert(__builtin_offsetof(struct percpu, u_rsp)    == 56,  "u_rsp @56");
_Static_assert(__builtin_offsetof(struct percpu, u_rip)    == 64,  "u_rip @64");
_Static_assert(__builtin_offsetof(struct percpu, u_rbx)    == 72,  "u_rbx @72");
_Static_assert(__builtin_offsetof(struct percpu, u_rbp)    == 80,  "u_rbp @80");
_Static_assert(__builtin_offsetof(struct percpu, u_r12)    == 88,  "u_r12 @88");
_Static_assert(__builtin_offsetof(struct percpu, u_r13)    == 96,  "u_r13 @96");
_Static_assert(__builtin_offsetof(struct percpu, u_r14)    == 104, "u_r14 @104");
_Static_assert(__builtin_offsetof(struct percpu, u_r15)    == 112, "u_r15 @112");
_Static_assert(__builtin_offsetof(struct percpu, u_rflags) == 120, "u_rflags @120");

static void gs_base_set(struct percpu *p) {
    uint64_t base = (uint64_t)(uintptr_t)p;
    __asm__ volatile("wrmsr" :: "c"(MSR_GS_BASE),
                     "a"((uint32_t)base), "d"((uint32_t)(base >> 32)));
}

void percpu_init_cpu(uint32_t cpu_idx) {
    if (cpu_idx >= PERCPU_MAX)
        return;
    struct percpu *p = &g_percpu[cpu_idx];
    p->self    = p;
    p->cpu_idx = cpu_idx;
    p->apic_id = lapic_id();
    p->present = 1;
    gs_base_set(p);
}

/* DDR-SMP-3b D3: the scheduler ticks long before ACPI/APIC init, and
 * current_thread is now %gs-relative — claim slot 0 for the BSP at kmain top
 * (identity unknown yet; percpu_init_bsp completes it). */
void percpu_init_early(void) {
    struct percpu *p = &g_percpu[0];
    p->self    = p;
    p->present = 1;
    p->is_bsp  = 1;                /* cap-2b D4: the BSP; survives the migration copy */
    gs_base_set(p);
}

void percpu_init_bsp(void) {
    uint32_t id = lapic_id();
    uint32_t ridx = 0;
    for (unsigned i = 0; i < lapic_cpu_count() && i < PERCPU_MAX; i++)
        if (lapic_apic_id_at(i) == id) {
            ridx = i;
            break;
        }
    if (ridx != 0) {
        /* Migrate the early slot-0 claim (incl. live current/kstack_top) to the
         * BSP's roster slot so an idx-0 AP can never collide (DDR-SMP-3b D3). */
        g_percpu[ridx] = g_percpu[0];
        g_percpu[ridx].self = &g_percpu[ridx];
        g_percpu[0].present = 0;
        gs_base_set(&g_percpu[ridx]);
        /* DDR-SMP-3c-cap-1 D4: the BSP LTR'd TSS[0] (selector 0x28) at boot; now
         * that its cpu_idx is ridx, re-home TR onto TSS[ridx] so tss_set_rsp0
         * (which indexes cpu_idx) and the CPU's live TR agree. Dormant on QEMU
         * (BSP roster index is 0), but correctness must not depend on that. */
        tss_init_cpu(ridx, 0);
    }
    g_percpu[ridx].cpu_idx = ridx;
    g_percpu[ridx].apic_id = id;
    g_percpu[ridx].present = 1;
}

struct percpu *percpu_get(uint32_t cpu_idx) {
    return (cpu_idx < PERCPU_MAX) ? &g_percpu[cpu_idx] : 0;
}

struct percpu *this_cpu(void) {
    struct percpu *p;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(p));   /* self pointer (DDR-SMP-3a) */
    return p;
}
