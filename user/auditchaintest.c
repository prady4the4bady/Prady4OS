/* user/auditchaintest.c — DDR-842 audit hash-chain gate (NSI 93).
 *
 * TWO GATES, one probe, and the pair is the whole design:
 *
 *   smoke-auditchain         QEMU_PROBES=auditchain
 *                            -> verify returns 0, chain INTACT
 *   smoke-auditchain-tamper  QEMU_PROBES=auditchain,audittamper
 *                            -> verify returns -EACCES, and reports WHICH index
 *
 * The tamper arm carries the feature. Ring 3 has no write path into the audit
 * log — that is exactly what S5 asserts — so the corruption is injected in the
 * kernel behind a DDR-804 probe flag. Without that arm, a verify() that returned
 * 0 unconditionally would pass, and the gate would be asserting nothing.
 *
 * This probe reports what it OBSERVED rather than asserting a fixed verdict, so
 * one binary serves both gates and the Makefile sentinels decide which outcome
 * each gate requires. A probe that hard-coded "expect intact" could not be
 * reused for the tamper arm, and two near-identical probes drift.
 *
 * NO WRITABLE GLOBALS (DDR-826): one R+X PT_LOAD, everything on the stack.
 */
#include <stdint.h>

#define SYS_WRITE         6
#define SYS_EXIT          4
#define SYS_VERIFY_AUDIT 93

#define EPERM   1
#define EACCES 13

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
    char b[24]; int i = 23; int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    b[i--] = 0;
    if (!u) b[i--] = '0';
    while (u) { b[i--] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) b[i--] = '-';
    wr(&b[i + 1]);
}
static void fail(const char *why, long rc) {
    wr("AUDITCHAIN FAIL: "); wr(why); wr(" rc="); wrint(rc); wr("\r\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    uint32_t bad = 0xFFFFFFFFu;
    long r = nsi(SYS_VERIFY_AUDIT, (long)&bad, 0, 0);

    if (r == -EPERM)
        fail("verify denied — this probe must run sovereign", r);

    if (r == 0) {
        wr("PRADYOS_AUDITCHAIN_INTACT_OK\r\n");
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }

    if (r == -EACCES) {
        /* The index must have been written. A verifier that reported breakage
         * without saying WHERE would leave an operator unable to find the event
         * being hidden, which is the only reason to verify at all. */
        if (bad == 0xFFFFFFFFu)
            fail("chain reported broken but no index was written", (long)bad);
        wr("PRADYOS_AUDITCHAIN_TAMPER_DETECTED_OK index=");
        wrint((long)bad);
        wr("\r\n");
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }

    fail("unexpected verify result", r);
    for (;;) { }        /* fail() exits; the compiler cannot prove it, and
                         * -Winvalid-noreturn is an error here. */
}
