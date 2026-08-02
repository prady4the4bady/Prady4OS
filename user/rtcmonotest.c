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

/* DDR-796 addendum: the first version ran a flat 20,000 samples and never
 * completed on a TCG CI runner — PRADYOS_RTC_MONO_OK appeared 0 times in an
 * entire CI job, and the probe starved later boot work enough to push
 * smoke-setname past its window (run 30405322967).
 *
 * Two mistakes, both mine, and both the same lesson this tree keeps teaching:
 * an ITERATION count mis-sizes on hosts of wildly different speed, and each
 * SYS_CLOCK now takes an IRQ-off spinlock that can spin ~2 ms during an RTC
 * update tick — so 20,000 of them is not a bounded cost.
 *
 * Now bounded by BOTH: stop after the wall clock advances RUN_SECONDS, or after
 * MAX_SAMPLES, whichever comes first. On a fast host that is a hard hammering;
 * on a slow one it exits promptly and samples less. Detection is probabilistic
 * either way — the A/B in DDR-796 is what proves it can detect the defect. */
#define MAX_SAMPLES  4000
#define RUN_SECONDS  3

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

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    long prev = nsi(SYS_CLOCK, 0, 0, 0);
    if (prev < 0) {
        wr("RTC_MONO FAIL: SYS_CLOCK returned an error\n");
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }

    long violations = 0;
    long started = prev;
    for (long i = 0; i < MAX_SAMPLES; i++) {
        long now = nsi(SYS_CLOCK, 0, 0, 0);

        /* Wall-clock bound, checked from the same reading we are about to
         * validate — no extra syscall, and it costs nothing when the clock is
         * behaving. A real midnight wrap ends the run rather than being treated
         * as elapsed time. */
        if (now >= started && now - started >= RUN_SECONDS)
            break;

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
