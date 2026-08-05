/* user/spawndepthtest.c — DDR-838 spawn-depth cap gate.
 *
 * The founding process is spawned as an AGENT. It forks a chain; each generation
 * forks again. The cap must allow depths 1, 2 and 3 and refuse the fourth with
 * -EAGAIN.
 *
 * BOTH halves are asserted, deliberately. A cap that refuses EVERY fork also
 * "bounds replication", and would pass a gate that only looked for a refusal —
 * so depths 1-3 succeeding is as much the test as generation 4 failing. The
 * depth at which the refusal lands is reported by value, so a cap firing at the
 * wrong generation fails rather than passing as "something was refused".
 *
 * Only the deepest generation reports; every other generation exits immediately
 * after forking. Children never loop: a runaway chain inside a gate that is
 * TESTING a runaway-chain limit would be a poor way to find out the limit is
 * broken.
 *
 * NO WRITABLE GLOBALS (DDR-826): one R+X PT_LOAD, everything on the stack.
 */
#include <stdint.h>

#define SYS_WRITE  6
#define SYS_EXIT   4
#define SYS_FORK   15

#define EAGAIN 11

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}
static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

static void wrint(long v) {
    char b[24];
    int i = 23;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    b[i--] = 0;
    if (!u) b[i--] = '0';
    while (u) { b[i--] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) b[i--] = '-';
    wr(&b[i + 1]);
}

static void fail(const char *why, long depth, long rc) {
    wr("SPAWNDEPTH FAIL: ");
    wr(why);
    wr(" depth=");
    wrint(depth);
    wr(" rc=");
    wrint(rc);
    wr("\r\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    /* depth 0 is the founding agent; each successful fork descends one level. */
    long depth = 0;

    for (;;) {
        long r = nsi(SYS_FORK, 0, 0, 0);

        if (r < 0) {
            /* A refusal. It must be -EAGAIN and it must land at depth 3 — the
             * fourth generation is the one being refused. */
            if (r != -EAGAIN)
                fail("fork refused with the wrong errno", depth, r);
            if (depth != 3)
                fail("cap fired at the wrong depth", depth, r);

            wr("PRADYOS_SPAWNDEPTH_OK depth=");
            wrint(depth);
            wr("\r\n");
            nsi(SYS_EXIT, 0, 0, 0);
            for (;;) { }
        }

        if (r > 0) {
            /* Parent: this generation did its job. Exit so the chain does not
             * fan out — each level forks exactly once. */
            nsi(SYS_EXIT, 0, 0, 0);
            for (;;) { }
        }

        /* Child: one level deeper, go again. */
        depth++;

        if (depth > 3)
            fail("fork succeeded past the cap", depth, 0);
    }
}
