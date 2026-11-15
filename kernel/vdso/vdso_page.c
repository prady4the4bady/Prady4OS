/* kernel/vdso/vdso_page.c — vDSO shared timekeeping frame (IMP-C). */
#include "vdso_page.h"
#include "pmm.h"          /* pmm_alloc_page, PAGE_SIZE */
#include "vmm.h"          /* vmm_map_in, VMM_USER/VMM_NX, PTE_SW_SHARED */
#include "string.h"       /* memset */
#include "console.h"      /* kputs (panic path) */
#include "irq.h"          /* g_ticks: fallback before the TSC is calibrated */
#include "rtc.h"          /* rtc_now: CLOCK_REALTIME base (DDR-1188 K6)    */

volatile vdso_data_t *vdso_data;     /* NULL until vdso_init (PIT handler guards on it) */
uint64_t vdso_page_phys;

void vdso_init(void) {
    uint64_t frame = pmm_alloc_page();
    if (!frame) {
        kputs("\r\n*** NEXUS PANIC: vdso_init out of memory ***\r\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    memset((void *)(uintptr_t)frame, 0, PAGE_SIZE);   /* clears the KASAN poison too */
    vdso_page_phys = frame;
    vdso_data = (volatile vdso_data_t *)(uintptr_t)frame;   /* identity map = kernel RW */
}

void vdso_map_user(uint64_t cr3) {
    if (!vdso_page_phys)
        return;
    /* Read-only (no VMM_RW), non-executable (VMM_NX), user, and SW_SHARED so the
     * address-space teardown / fork paths share this one frame instead of freeing
     * or copying it. map_core adds PRESENT. */
    vmm_map_in(cr3, VDSO_USER_VA, vdso_page_phys,
               VMM_USER | VMM_NX | PTE_SW_SHARED);
}

/* ---- DDR-1188 K7 (C1.5 P1): the TSC clock -------------------------------- */

static inline uint64_t vt_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian; H. Hinnant's algorithm). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void vdso_time_init(uint64_t tsc_hz) {
    if (!vdso_data || !tsc_hz)
        return;
    /* mult = 1e9 * 2^shift / tsc_hz, the largest shift <= 24 that fits 32 bits. */
    uint32_t shift = 24;
    uint64_t mult;
    for (;;) {
        mult = (1000000000ull << shift) / tsc_hz;   /* 1e9 << 24 < 2^64 */
        if (mult <= 0xFFFFFFFFull || shift == 0)
            break;
        shift--;
    }
    if (mult > 0xFFFFFFFFull)
        return;                         /* TSC below 1 Hz: leave the PIT fallback */
    struct rtc_time rt;
    rtc_now(&rt);
    int64_t days = days_from_civil(rt.year, rt.month, rt.day);
    int64_t sec = days * 86400 + rt.hour * 3600 + rt.minute * 60 + rt.second;
    uint64_t mono0 = vdso_data->wall_time_ns;   /* continue from the PIT clock */

    vdso_data->seq++;                   /* odd: update in progress */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    vdso_data->shift = shift;
    vdso_data->mult = (uint32_t)mult;
    vdso_data->tsc_base = vt_rdtsc();
    vdso_data->ns_base = mono0;
    vdso_data->realtime_base = (sec > 0 ? (uint64_t)sec * 1000000000ull : 0) - mono0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    vdso_data->seq++;                   /* even: complete */
}

/* Called from timer_tick on the BSP, the only writer. The reader's formula,
 * evaluated at now, becomes the new ns_base: the clock cannot step back. */
void vdso_time_update(void) {
    if (!vdso_data || !vdso_data->mult)
        return;
    uint64_t now = vt_rdtsc();
    uint64_t delta = now - vdso_data->tsc_base;
    vdso_data->seq++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    (void)delta;
    vdso_data->ns_base  = vdso_data->ns_base + 10000000ull;   /* MUTANT C1 */
    vdso_data->tsc_base = now;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    vdso_data->seq++;
}

uint64_t ktime_mono_ns(void) {
    if (!vdso_data)
        return 0;
    for (;;) {
        uint32_t s0 = vdso_data->seq;      /* volatile; packed: no &member */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (s0 & 1u)
            continue;
        uint32_t mult = vdso_data->mult;
        uint64_t v;
        if (!mult) {
            v = vdso_data->wall_time_ns;            /* before vdso_time_init */
        } else {
            uint64_t base = vdso_data->ns_base, tb = vdso_data->tsc_base;
            uint32_t sh = vdso_data->shift;
            uint64_t now = vt_rdtsc();
            uint64_t d = now > tb ? now - tb : 0;   /* a CPU behind tsc_base: hold */
            v = base + (uint64_t)(((unsigned __int128)d * mult) >> sh);
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (vdso_data->seq == s0)
            return v;
    }
}

uint64_t ktime_real_ns(void) {
    uint64_t rb = vdso_data ? vdso_data->realtime_base : 0;
    return rb + ktime_mono_ns();
}
