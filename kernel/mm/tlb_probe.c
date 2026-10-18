/* kernel/mm/tlb_probe.c — smoke-tlbshoot in-kernel probe (DDR-1177, C1.2).
 *
 * Owner decision #45/6251086742 item 1: the C1.2 exit gate is this probe,
 * arms A-C, kernel code on 4 CPUs. Arms D (mprotect) and E (user space) are
 * C1.4 exit criteria.
 *
 * Setup. The BSP is the only initiator. It maps a kernel-half test page and
 * rotates it through three frames filled with 0xA0, 0xA1, 0xA2 (phase p maps
 * frame p % 3), calling tlb_shoot(0, va, 1) after every remap, then publishing
 * the phase. Three frames, not two: between the remap and the publish a correct
 * reader legitimately sees the NEXT frame under the old phase, so the valid set
 * for phase p is {p, p+1}. With two frames p+1 == p-1 and the check would be
 * vacuous; run 39136618235 counted that legitimate window as stale=4871737.
 * Every other ready CPU runs a reader job that keeps the page hot in its TLB.
 *
 *   A  stale  : a reader that had already read phase p saw frame p-1, which
 *               is unmapped and flushed everywhere before p is published. Kills M1/M2 only if
 *               the emulator can hold a stale cross-CPU entry (may be vacuous
 *               under TCG; item 2).
 *   B  early  : after tlb_shoot returned, a CPU in the request's target mask
 *               had not yet serviced that generation. Kills M2 (no ack wait).
 *   C  missed : a ready non-self CPU absent from the target mask of a
 *               kernel-half request (cr3 == 0 must target every ready CPU).
 *               Kills M3 (wrong mask).
 */
#include <stdint.h>
#include "tlb.h"
#include "percpu.h"
#include "pmm.h"
#include "vmm.h"
#include "smp.h"
#include "irq.h"
#include "console.h"

#define TP_VA    0xFFFF800000200000ull   /* PML4 slot 256, beside vmm_test's */
#define TP_ITERS 20000u

static volatile uint64_t tp_phase;
static volatile uint32_t tp_stop;
static volatile uint64_t tp_stale, tp_reads;

static void tp_reader(void) {
    __asm__ volatile("sti");             /* the 0xF0 IPI must be deliverable */
    volatile uint8_t *p = (volatile uint8_t *)TP_VA;
    uint64_t stale = 0, reads = 0;
    while (!__atomic_load_n(&tp_stop, __ATOMIC_ACQUIRE)) {
        uint64_t ph = __atomic_load_n(&tp_phase, __ATOMIC_ACQUIRE);
        uint8_t v = *p;
        reads++;
        if (ph && v == (uint8_t)(0xA0 + (ph - 1) % 3))
            stale++;
    }
    __atomic_add_fetch(&tp_stale, stale, __ATOMIC_RELAXED);
    __atomic_add_fetch(&tp_reads, reads, __ATOMIC_RELAXED);
}

static void tp_fill(uint64_t frame, uint8_t b) {
    vmm_map(TP_VA, frame, VMM_RW);       /* local invlpg inside */
    volatile uint8_t *p = (volatile uint8_t *)TP_VA;
    for (unsigned i = 0; i < 4096; i++)
        p[i] = b;
}

static inline uint64_t tp_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void tp_line(const char *s, uint64_t a, const char *s2, uint64_t b) {
    kline k; kline_init(&k);
    kline_s(&k, s); kline_d(&k, a);
    if (s2) { kline_s(&k, s2); kline_d(&k, b); }
    kline_s(&k, "\r\n"); kline_emit(&k);
}

void tlbshoot_probe(void) {
    struct percpu *self = this_cpu();
    uint64_t fr[3] = { pmm_alloc_page(), pmm_alloc_page(), pmm_alloc_page() };
    if (!self || !fr[0] || !fr[1] || !fr[2]) {
        tp_line("PRADYOS_TLBSHOOT FAIL setup=", 1, 0, 0);
        return;
    }
    tp_fill(fr[2], 0xA2);
    tp_fill(fr[1], 0xA1);
    tp_fill(fr[0], 0xA0);                /* phase 0 maps frame 0 */
    tlb_shoot(0, TP_VA, 1);

    uint64_t expect = 0;                 /* ready CPUs other than self */
    for (uint32_t i = 0; i < PERCPU_MAX; i++) {
        struct percpu *p = percpu_get(i);
        if (!p || p == self || !p->present ||
            !__atomic_load_n(&p->shoot_ready, __ATOMIC_ACQUIRE))
            continue;
        expect |= 1ull << p->cpu_idx;
        smp_run_on(p->cpu_idx, tp_reader);
    }
    uint64_t t0 = g_ticks;
    while (g_ticks < t0 + 5)             /* let readers warm their TLBs */
        __asm__ volatile("pause");

    uint64_t early = 0, missed = 0, targets = 0, cyc = 0;
    uint32_t it;
    for (it = 0; it < TP_ITERS; it++) {
        uint64_t ph = it + 1;
        vmm_map(TP_VA, fr[ph % 3], VMM_RW);
        uint64_t c0 = tp_rdtsc();
        tlb_shoot(0, TP_VA, 1);
        cyc += tp_rdtsc() - c0;
        uint64_t gen, mask;
        tlb_probe_last(&gen, &mask);
        for (uint32_t i = 0; i < PERCPU_MAX; i++) {
            if (!(mask & (1ull << i)))
                continue;
            struct percpu *p = percpu_get(i);
            if (__atomic_load_n(&p->shoot_seen_gen, __ATOMIC_ACQUIRE) < gen)
                early++;
        }
        missed += (uint64_t)__builtin_popcountll(expect & ~mask);
        targets = (uint64_t)__builtin_popcountll(mask);
        __atomic_store_n(&tp_phase, ph, __ATOMIC_RELEASE);
    }

    __atomic_store_n(&tp_stop, 1u, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < PERCPU_MAX; i++) {
        if (!(expect & (1ull << i)))
            continue;
        uint64_t d = g_ticks + 200;
        while (!smp_job_done(i) && g_ticks < d)
            __asm__ volatile("pause");
    }
    vmm_unmap(TP_VA);
    tlb_shoot(0, TP_VA, 1);
    for (unsigned i = 0; i < 3; i++)
        pmm_free_page(fr[i]);

    tp_line("PRADYOS_TLBSHOOT_A stale=", tp_stale, " iters=", it);
    tp_line("PRADYOS_TLBSHOOT_B early=", early, 0, 0);
    tp_line("PRADYOS_TLBSHOOT_C missed=", missed, " targets=", targets);
    tp_line("[tlbshoot] reads=", tp_reads, " cyc_per_shoot=", it ? cyc / it : 0);
    tp_line("PRADYOS_TLBSHOOT_DONE cpus=", targets + 1, 0, 0);
}
