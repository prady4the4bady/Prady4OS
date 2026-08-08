/* user/benchtest.c — ring-3 cycle benchmark for the perf-critical asm paths.
 * Group 8 items 44/45, DDR-870.
 *
 * WHAT IS MEASURED
 *   syscall  — SYS_GETPID round trip: SYSCALL -> syscall_entry.asm -> dispatch
 *              -> SYSRET. The cheapest call in the table, so the figure is the
 *              ENTRY PATH cost and not the handler's.
 *   yield    — SYS_YIELD, which additionally runs schedule() and, when another
 *              runnable thread exists, context_switch.asm.
 *
 * WHAT THE NUMBER IS WORTH, stated up front because it is easy to over-read.
 * Under QEMU TCG the guest RDTSC counts EMULATED time, not host cycles and not
 * the cycles a real CPU would spend. A dynamically-translated `swapgs` is not
 * one hardware `swapgs`. So these figures are:
 *   - VALID for regression: a large jump means the path got longer.
 *   - INVALID as an absolute hardware claim.
 * The annotations in the .asm files therefore carry the exact instruction and
 * memory-traffic counts, which ARE hardware-meaningful, alongside this figure
 * labelled as emulated. Quoting a TCG number as "N cycles on x86_64" would be
 * inventing precision the measurement cannot support.
 *
 * METHOD. The minimum over many iterations, not the mean: the minimum is the
 * closest thing to an uninterrupted path, while the mean folds in timer ticks
 * and scheduler preemption that have nothing to do with the code under test.
 */

#include "pradyos.h"

#define ITERS   2000
#define WARMUP   200

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { pradyos_syscall(SYS_WRITE, 1, (long)s, slen(s)); }

static void wrnum(unsigned long v) {
    char b[24];
    int i = 23;
    b[i--] = 0;
    if (!v) b[i--] = '0';
    while (v) { b[i--] = (char)('0' + (v % 10)); v /= 10; }
    wr(&b[i + 1]);
}

__attribute__((noreturn)) static void fail(const char *why) {
    wr("BENCH FAIL: "); wr(why); wr("\n");
    pradyos_syscall(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

/* Serialising read: LFENCE stops the TSC read drifting across the work being
 * timed. Without it an out-of-order core may retire RDTSC early and the
 * measurement silently shrinks. */
static inline unsigned long rdtsc_serial(void) {
    unsigned int lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((unsigned long)hi << 32) | lo;
}

static unsigned long bench_min(long nsi_num) {
    unsigned long best = ~0UL;
    for (int i = 0; i < WARMUP; i++)
        (void)pradyos_syscall(nsi_num, 0, 0, 0);
    for (int i = 0; i < ITERS; i++) {
        unsigned long t0 = rdtsc_serial();
        (void)pradyos_syscall(nsi_num, 0, 0, 0);
        unsigned long t1 = rdtsc_serial();
        unsigned long d = t1 - t0;
        if (d < best) best = d;
    }
    return best;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    wr("[bench] probe entered\n");

    /* An empty-loop baseline: the RDTSC pair itself is not free, and reporting
     * a syscall cost that silently includes it would overstate the path. */
    unsigned long base = ~0UL;
    for (int i = 0; i < ITERS; i++) {
        unsigned long t0 = rdtsc_serial();
        unsigned long t1 = rdtsc_serial();
        unsigned long d = t1 - t0;
        if (d < base) base = d;
    }

    unsigned long sc = bench_min(SYS_GETPID);
    unsigned long yl = bench_min(SYS_YIELD);

    if (sc <= base) fail("syscall measured at or below the RDTSC baseline");

    wr("PRADYOS_BENCH rdtsc_base="); wrnum(base);
    wr(" syscall_getpid=");          wrnum(sc - base);
    wr(" syscall_yield=");           wrnum(yl > base ? yl - base : 0);
    wr(" iters=");                   wrnum(ITERS);
    wr(" (QEMU TCG emulated ticks - NOT hardware cycles)\n");

    wr("PRADYOS_BENCH_OK\n");
    pradyos_syscall(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
