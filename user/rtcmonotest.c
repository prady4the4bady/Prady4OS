/* user/rtcmonotest.c — SYS_CLOCK monotonicity probe (DDR-796, BUG-1).
 *
 * Tests the invariant directly instead of re-running the symptom. BUG-1 surfaced
 * three inferential steps away from its cause — a metrics probe reporting "agent
 * never observed as scheduled" because its 120-second window had collapsed to
 * zero — which is why it went unattributed for so long.
 *
 * The defect: CMOS access is a two-port sequence (0x70 selects a register, 0x71
 * reads it) over state owned by the chipset, not the CPU. Unserialised, two CPUs
 * interleave and each reads the other's register, so the wall clock can jump in
 * either direction.
 *
 * This probe reads SYS_CLOCK in a tight loop and asserts the reading never goes
 * BACKWARDS. A single backwards step is the whole bug. Runs long enough to make
 * the interleave likely under -smp 4, and prints a bounded number of violations
 * so a broken clock cannot flood the log.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_WRITE  6
#define SYS_YIELD  3
#define SYS_EXIT   4
#define SYS_CLOCK  57

#define SAMPLES    20000

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

static void wrdec(long v) {
    char buf[24];
    int i = 23;
    buf[i--] = 0;
    if (v == 0) buf[i--] = '0';
    while (v > 0 && i >= 0) { buf[i--] = (char)('0' + (v % 10)); v /= 10; }
    wr(&buf[i + 1]);
}

__attribute__((noreturn)) void _start(void) {
    long prev = nsi(SYS_CLOCK, 0, 0, 0);
    if (prev < 0) {
        wr("RTC_MONO FAIL: SYS_CLOCK returned an error\n");
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }

    long violations = 0;
    for (long i = 0; i < SAMPLES; i++) {
        long now = nsi(SYS_CLOCK, 0, 0, 0);

        /* A real midnight wrap goes 86399 -> 0. Anything else that decreases is
         * the race. Treating every decrease as a wrap is exactly the mistake the
         * metrics probe made, so the wrap is recognised narrowly. */
        int real_wrap = (prev > 86000 && now < 400);
        if (now < prev && !real_wrap) {
            if (violations < 5) {          /* bounded: a broken clock must not flood */
                wr("RTC_MONO FAIL: clock went backwards ");
                wrdec(prev);
                wr(" -> ");
                wrdec(now);
                wr("\n");
            }
            violations++;
        }
        prev = now;
        if ((i & 0x3F) == 0)
            nsi(SYS_YIELD, 0, 0, 0);       /* let other CPUs interleave */
    }

    if (violations) {
        wr("RTC_MONO FAIL: total violations=");
        wrdec(violations);
        wr("\n");
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }

    wr("PRADYOS_RTC_MONO_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
