/* user/metrictest.c — F#68 metric-region probe (DDR-795).
 *
 * Two assertions, in order, and the order matters:
 *
 *   1. the sealed objective-function root page is MAPPED and READABLE from
 *      ring 3 — prints METRIC_READ_OK;
 *   2. a store to it FAULTS — the kernel converts the ring-3 #PF into a clean
 *      process kill, so nothing after the store ever runs.
 *
 * If the store were to succeed, execution continues and the probe prints
 * METRIC_WX_FAIL, which the gate declares forbidden. Printing the read sentinel
 * FIRST is what stops the gate passing by accident: a probe that crashed before
 * reaching the store would fail the required sentinel instead of silently
 * looking like a successful W^X refusal.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_EXIT   4
#define SYS_WRITE  6

#define METRIC_USER_VA  0x00007FFFFFEFF000ull
#define METRIC_MAGIC    0x4D455452u      /* "METR" */
#define METRIC_VERSION  1u

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn)) static void fail(const char *why) {
    wr("METRIC FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn)) void _start(void) {
    volatile const unsigned int *page =
        (volatile const unsigned int *)METRIC_USER_VA;

    /* 1. Read. A fault here means the page was never mapped, which is a
     *    different defect from "the page is writable" and must not be
     *    reported as the same thing. */
    unsigned int magic   = page[0];
    unsigned int version = page[1];

    if (magic != METRIC_MAGIC)
        fail("magic mismatch — metric region not mapped or not stamped");
    if (version != METRIC_VERSION)
        fail("version mismatch — layout changed without a version bump");

    wr("METRIC_READ_OK\n");

    /* 2. Write. Must fault: the page is mapped read-only + NX into every user
     *    address space (DDR-795). The kernel's ring-3 fault path kills us. */
    volatile unsigned int *rw = (volatile unsigned int *)METRIC_USER_VA;
    rw[16] = 0xDEADBEEFu;             /* offset 64 — inside the reserved area */

    /* Unreachable if the mapping is honest. */
    wr("METRIC_WX_FAIL\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}
