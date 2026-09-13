/* user/shaketest.c — DDR-1052 FIPS 202 (SHA-3 / SHAKE) known-answer probe.
 *
 * Links kernel/crypto/keccak.c — the SAME SOURCE the kernel builds, compiled a
 * second time for ring 3, exactly as sha256test.c does for SHA-256. One
 * implementation, tested from userspace; a separate userspace copy could drift
 * from the kernel's and the gate would still pass.
 *
 * The vectors themselves live in kernel/crypto/keccak_kat.h and were generated
 * by Python's hashlib — an implementation that is not this one. That is what
 * makes this a KNOWN-ANSWER test rather than a round trip. A hash-then-check-
 * against-itself gate passes on any self-consistent wrong implementation, which
 * is the dead-arm class this project has hit repeatedly; for a signature scheme
 * the same trap is sign-then-verify, and it is why FIPS 204 conformance will
 * need pinned vectors too.
 */
#include "keccak.h"

#define SYS_WRITE 6
#define SYS_EXIT  4

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

static void wrdec(unsigned v) {
    char b[12];
    int i = 11;
    b[i--] = 0;
    if (!v) b[i--] = '0';
    while (v && i >= 0) { b[i--] = (char)('0' + (v % 10u)); v /= 10u; }
    wr(&b[i + 1]);
}

__attribute__((force_align_arg_pointer))
void _start(void) {
    int bad = keccak_selftest();
    if (bad != 0) {
        /* Name the failing vector. The index is not decoration: vectors 7-12
         * are the rate boundaries and 13-14 the multi-block squeeze, so which
         * one failed says which part of the sponge is wrong. */
        wr("PRADYOS_SHAKE_STUB first_bad_vector=");
        wrdec((unsigned)bad);
        wr("\n");
        wr("SHAKE FAIL\n");
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }
    wr("PRADYOS_SHAKE_VECTORS_OK n=15\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
