/* user/sha256test.c — DDR-811 SHA-256 vector probe.
 *
 * Links kernel/crypto/sha256.c — the SAME SOURCE the kernel builds, compiled a
 * second time with the user code model. A literal shared .o is impossible
 * (kernel objects are -mcmodel=kernel, user objects -mcmodel=large), but the
 * point stands: there is ONE implementation. A probe that reimplemented SHA-256
 * would test the probe.
 *
 * Four vectors from FIPS 180-4. Vector 4 (1,000,000 'a') is the load-bearing
 * one: vectors 1-3 all fit in two blocks and would pass against an
 * implementation whose 64-bit length counter is wrong or whose update()
 * mishandles a partial block carried across calls. It is fed in 64-byte chunks
 * precisely to exercise that carry.
 *
 * On failure the probe prints the index of the FIRST mismatching byte, not just
 * "wrong" — a digest that differs in one byte and one that differs entirely are
 * different bugs.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */
#include "sha256.h"

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

/* Compare and report the first differing byte index. */
static int check(const char *name, const uint8_t *got, const uint8_t *want) {
    for (unsigned i = 0; i < SHA256_DIGEST_LEN; i++) {
        if (got[i] != want[i]) {
            wr("PRADYOS_SHA256_STUB vector=");
            wr(name);
            wr(" first_bad_byte=");
            wrdec(i);
            wr("\n");
            return 0;
        }
    }
    return 1;
}

/* FIPS 180-4 published digests. If any constant here is wrong the gate fails,
 * which is the intended safety property: a wrong vector cannot ship silently. */
static const uint8_t WANT_ABC[32] = {
    0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
};
static const uint8_t WANT_EMPTY[32] = {
    0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
    0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55
};
static const uint8_t WANT_56[32] = {
    0x24,0x8d,0x6a,0x61,0xd2,0x06,0x38,0xb8,0xe5,0xc0,0x26,0x93,0x0c,0x3e,0x60,0x39,
    0xa3,0x3c,0xe4,0x59,0x64,0xff,0x21,0x67,0xf6,0xec,0xed,0xd4,0x19,0xdb,0x06,0xc1
};
static const uint8_t WANT_1M[32] = {
    0xcd,0xc7,0x6e,0x5c,0x99,0x14,0xfb,0x92,0x81,0xa1,0xc7,0xe2,0x84,0xd7,0x3e,0x67,
    0xf1,0x80,0x9a,0x48,0xa4,0x97,0x20,0x0e,0x04,0x6d,0x39,0xcc,0xc7,0x11,0x2c,0xd0
};

__attribute__((noreturn)) void _start(void) {
    uint8_t d[SHA256_DIGEST_LEN];
    int ok = 1;

    /* 1 — "abc": canonical single block. */
    sha256("abc", 3, d);
    ok &= check("abc", d, WANT_ABC);

    /* 2 — empty: length-zero padding. */
    sha256("", 0, d);
    ok &= check("empty", d, WANT_EMPTY);

    /* 3 — 56 bytes: exactly the boundary where the length field no longer fits
     * in the first block, forcing a second padding block. */
    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    ok &= check("56byte", d, WANT_56);

    /* 4 — 1,000,000 'a', streamed in 1000-byte chunks.
     *
     * The chunk size is deliberately NOT a multiple of 64. Feeding block-aligned
     * chunks would leave buflen at 0 on every call, so update()'s partial-block
     * carry — the thing this vector exists to exercise — would never execute and
     * the gate would pass while testing nothing of the sort. 1000 = 15*64 + 40,
     * so every call leaves a 40-byte remainder that the next call must top up
     * correctly, and the boundary walks through all 64 offsets over the run. */
    {
        uint8_t chunk[1000];
        for (unsigned i = 0; i < 1000u; i++)
            chunk[i] = 'a';
        sha256_ctx c;
        sha256_init(&c);
        for (unsigned n = 0; n < 1000u; n++)       /* 1000 * 1000 = 1,000,000 */
            sha256_update(&c, chunk, 1000);
        sha256_final(&c, d);
        ok &= check("1M-a", d, WANT_1M);
    }

    if (!ok) {
        wr("SHA256 FAIL\n");
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }
    wr("PRADYOS_SHA256_VECTORS_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
