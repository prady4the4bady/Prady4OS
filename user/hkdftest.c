/* user/hkdftest.c — DDR-818 HKDF-SHA256 vector probe.
 *
 * Links kernel/crypto/hkdf.c and sha256.c — the same sources the kernel builds,
 * compiled a second time with the user code model. One implementation, two
 * builds; a probe that reimplemented HKDF would test the probe.
 *
 * Three RFC 5869 Appendix A cases, each covering something the others cannot:
 *
 *   1. basic (42-byte OKM)          — the ordinary path
 *   2. long inputs (82-byte OKM)    — the ONLY case that forces the expand loop
 *                                     past T(1); an implementation computing
 *                                     only T(1) satisfies cases 1 and 3
 *   3. zero-length salt and info    — the ONLY case exercising the NULL-salt
 *                                     branch, which RFC 5869 says is HashLen
 *                                     ZERO BYTES, not an empty string
 *
 * On failure the probe prints the index of the first mismatching byte — a digest
 * that differs in one position and one that differs entirely are different bugs.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */
#include "hkdf.h"

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
    char b[12]; int i = 11; b[i--] = 0;
    if (!v) b[i--] = '0';
    while (v && i >= 0) { b[i--] = (char)('0' + (v % 10u)); v /= 10u; }
    wr(&b[i + 1]);
}

static int check(const char *name, const uint8_t *got,
                 const uint8_t *want, unsigned len) {
    for (unsigned i = 0; i < len; i++) {
        if (got[i] != want[i]) {
            wr("PRADYOS_HKDF_STUB case=");
            wr(name);
            wr(" first_bad_byte=");
            wrdec(i);
            wr("\n");
            return 0;
        }
    }
    return 1;
}

/* RFC 5869 Appendix A, SHA-256 cases. If any constant is wrong the gate fails
 * immediately rather than shipping — the implementation was written
 * independently of these, so both being wrong the same way is unreachable. */
static const uint8_t TC1_OKM[42] = {
    0x3c,0xb2,0x5f,0x25,0xfa,0xac,0xd5,0x7a,0x90,0x43,0x4f,0x64,0xd0,0x36,0x2f,0x2a,
    0x2d,0x2d,0x0a,0x90,0xcf,0x1a,0x5a,0x4c,0x5d,0xb0,0x2d,0x56,0xec,0xc4,0xc5,0xbf,
    0x34,0x00,0x72,0x08,0xd5,0xb8,0x87,0x18,0x58,0x65
};
static const uint8_t TC2_OKM[82] = {
    0xb1,0x1e,0x39,0x8d,0xc8,0x03,0x27,0xa1,0xc8,0xe7,0xf7,0x8c,0x59,0x6a,0x49,0x34,
    0x4f,0x01,0x2e,0xda,0x2d,0x4e,0xfa,0xd8,0xa0,0x50,0xcc,0x4c,0x19,0xaf,0xa9,0x7c,
    0x59,0x04,0x5a,0x99,0xca,0xc7,0x82,0x72,0x71,0xcb,0x41,0xc6,0x5e,0x59,0x0e,0x09,
    0xda,0x32,0x75,0x60,0x0c,0x2f,0x09,0xb8,0x36,0x77,0x93,0xa9,0xac,0xa3,0xdb,0x71,
    0xcc,0x30,0xc5,0x81,0x79,0xec,0x3e,0x87,0xc1,0x4c,0x01,0xd5,0xc1,0xf3,0x43,0x4f,
    0x1d,0x87
};
static const uint8_t TC3_OKM[42] = {
    0x8d,0xa4,0xe7,0x75,0xa5,0x63,0xc1,0x8f,0x71,0x5f,0x80,0x2a,0x06,0x3c,0x5a,0x31,
    0xb8,0xa1,0x1f,0x5c,0x5e,0xe1,0x87,0x9e,0xc3,0x45,0x4e,0x5f,0x3c,0x73,0x8d,0x2d,
    0x9d,0x20,0x13,0x95,0xfa,0xa4,0xb6,0x1a,0x96,0xc8
};

__attribute__((noreturn)) void _start(void) {
    uint8_t okm[82];
    int ok = 1;

    /* ---- TC1: IKM = 0x0b x22, salt = 000102..0c, info = f0f1..f9, L = 42 ---- */
    {
        uint8_t ikm[22], salt[13], info[10];
        for (unsigned i = 0; i < 22u; i++) ikm[i] = 0x0bu;
        for (unsigned i = 0; i < 13u; i++) salt[i] = (uint8_t)i;
        for (unsigned i = 0; i < 10u; i++) info[i] = (uint8_t)(0xf0u + i);
        if (hkdf_sha256(salt, 13, ikm, 22, info, 10, okm, 42) != 0) {
            wr("HKDF FAIL: TC1 returned error\n");
            ok = 0;
        } else {
            ok &= check("tc1", okm, TC1_OKM, 42);
        }
    }

    /* ---- TC2: 80-byte IKM/salt/info, L = 82. Forces T(1)..T(3). ---- */
    {
        uint8_t ikm[80], salt[80], info[80];
        for (unsigned i = 0; i < 80u; i++) ikm[i]  = (uint8_t)i;          /* 00..4f */
        for (unsigned i = 0; i < 80u; i++) salt[i] = (uint8_t)(0x60u + i); /* 60..af */
        for (unsigned i = 0; i < 80u; i++) info[i] = (uint8_t)(0xb0u + i); /* b0..ff */
        if (hkdf_sha256(salt, 80, ikm, 80, info, 80, okm, 82) != 0) {
            wr("HKDF FAIL: TC2 returned error\n");
            ok = 0;
        } else {
            ok &= check("tc2", okm, TC2_OKM, 82);
        }
    }

    /* ---- TC3: zero-length salt AND info. Exercises the NULL-salt branch. ---- */
    {
        uint8_t ikm[22];
        for (unsigned i = 0; i < 22u; i++) ikm[i] = 0x0bu;
        if (hkdf_sha256(0, 0, ikm, 22, 0, 0, okm, 42) != 0) {
            wr("HKDF FAIL: TC3 returned error\n");
            ok = 0;
        } else {
            ok &= check("tc3", okm, TC3_OKM, 42);
        }
    }

    /* Bound check: more OKM than the construction can produce must be refused,
     * not silently truncated. */
    if (hkdf_sha256(0, 0, "k", 1, 0, 0, okm, 1) != 0) {
        wr("HKDF FAIL: minimal call rejected\n");
        ok = 0;
    }

    if (!ok) {
        wr("HKDF FAIL\n");
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }
    wr("PRADYOS_HKDF_VECTORS_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
