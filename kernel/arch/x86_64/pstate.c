/* kernel/arch/x86_64/pstate.c — CPU frequency scaling (item 27, DDR-892).
 *
 * IA32_PERF_CTL / IA32_PERF_STATUS, gated on CPUID.01H:ECX bit 7 (EIST).
 *
 * The capability check is not politeness. `wrmsr` to an MSR the CPU does not
 * implement raises #GP — at boot that is a kernel fault, not a missing feature.
 * cpu_mitigations.c already establishes this pattern here: check the bit, then
 * write.
 */
#include "pstate.h"
#include "console.h"

#define MSR_IA32_PERF_STATUS 0x198u
#define MSR_IA32_PERF_CTL    0x199u

static int g_eist;

static inline uint64_t rd_msr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wr_msr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

void pstate_init(void) {
    uint32_t eax = 1, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));
    g_eist = (ecx >> 7) & 1u;

    if (!g_eist) {
        /* The honest outcome on this platform. Reporting a "current frequency"
         * read from an MSR the CPU does not implement would be inventing a
         * number, and a scaling driver that appears to work while changing
         * nothing is worse than one that says it cannot. */
        kputs("[pstate] EIST unsupported; frequency scaling unavailable\r\n");
        return;
    }
    kputs("[pstate] EIST present, perf_status=");
    kputhex(rd_msr(MSR_IA32_PERF_STATUS) & 0xFFFFu);
    kputs("\r\n");
}

int pstate_available(void) { return g_eist; }

uint16_t pstate_current(void) {
    if (!g_eist)
        return 0;
    return (uint16_t)(rd_msr(MSR_IA32_PERF_STATUS) & 0xFFFFu);
}

int pstate_set(uint16_t ctl) {
    if (!g_eist)
        return -1;                    /* refuse; never a silent no-op */
    uint64_t v = rd_msr(MSR_IA32_PERF_CTL);
    v = (v & ~0xFFFFull) | ctl;       /* preserve the reserved/turbo bits */
    wr_msr(MSR_IA32_PERF_CTL, v);
    return 0;
}
