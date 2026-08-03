/* user/sha512test.c — DDR-821 SHA-512 vector probe (FIPS 180-4).
 *
 * Links kernel/crypto/sha512.c — the same source the kernel builds, compiled a
 * second time with the user code model. A probe that reimplemented SHA-512
 * would test the probe.
 *
 * Four vectors, each reaching something the others cannot:
 *
 *   1. empty message   — the length-0 padding path. Nothing else reaches it,
 *                        and it is where an off-by-one in the pad loop hides.
 *   2. "abc"           — the ordinary one-block path.
 *   3. 112-byte msg    — 112 = 128 - 16, i.e. the message ends EXACTLY where
 *                        the 128-bit length field must go, forcing the
 *                        two-block path. An implementation that writes the
 *                        length into the first block passes 1 and 2 and fails
 *                        only here.
 *   4. 1M 'a' streamed — fed in 1000-byte chunks (1000 = 7*128 + 104), NOT
 *                        block-aligned. A 128-byte chunk size never runs the
 *                        partial-block carry in sha512_update, so it would
 *                        pass against a broken carry. This is the DDR-811
 *                        lesson: choose chunk size by which code path it
 *                        reaches, not by convenience.
 *
 * Constants are RECALLED, not fetched (same caveat as DDR-811/818/819/820).
 * The gate compares raw bytes, so a wrong constant fails immediately, and the
 * implementation was written independently of them.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 * force_align_arg_pointer is mandatory and asserted by ci-start-align-check
 * (DDR-823) — a process entry point gets RSP 16-byte aligned while the compiler
 * assumes the call convention's RSP = 8 (mod 16).
 */
#include "sha512.h"

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

static int check(const char *name, const uint8_t *got, const uint8_t *want) {
    for (unsigned i = 0; i < SHA512_DIGEST_LEN; i++) {
        if (got[i] != want[i]) {
            wr("PRADYOS_SHA512_STUB case=");
            wr(name);
            wr(" first_bad_byte=");
            wrdec(i);
            wr("\n");
            return 0;
        }
    }
    return 1;
}

static const uint8_t V_EMPTY[64] = {
    0xcf,0x83,0xe1,0x35,0x7e,0xef,0xb8,0xbd,0xf1,0x54,0x28,0x50,0xd6,0x6d,0x80,0x07,
    0xd6,0x20,0xe4,0x05,0x0b,0x57,0x15,0xdc,0x83,0xf4,0xa9,0x21,0xd3,0x6c,0xe9,0xce,
    0x47,0xd0,0xd1,0x3c,0x5d,0x85,0xf2,0xb0,0xff,0x83,0x18,0xd2,0x87,0x7e,0xec,0x2f,
    0x63,0xb9,0x31,0xbd,0x47,0x41,0x7a,0x81,0xa5,0x38,0x32,0x7a,0xf9,0x27,0xda,0x3e };

static const uint8_t V_ABC[64] = {
    0xdd,0xaf,0x35,0xa1,0x93,0x61,0x7a,0xba,0xcc,0x41,0x73,0x49,0xae,0x20,0x41,0x31,
    0x12,0xe6,0xfa,0x4e,0x89,0xa9,0x7e,0xa2,0x0a,0x9e,0xee,0xe6,0x4b,0x55,0xd3,0x9a,
    0x21,0x92,0x99,0x2a,0x27,0x4f,0xc1,0xa8,0x36,0xba,0x3c,0x23,0xa3,0xfe,0xeb,0xbd,
    0x45,0x4d,0x44,0x23,0x64,0x3c,0xe8,0x0e,0x2a,0x9a,0xc9,0x4f,0xa5,0x4c,0xa4,0x9f };

/* 112 bytes = 128 - 16: ends exactly where the 128-bit length field must go. */
static const char MSG112[] =
    "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
    "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
static const uint8_t V_112[64] = {
    0x8e,0x95,0x9b,0x75,0xda,0xe3,0x13,0xda,0x8c,0xf4,0xf7,0x28,0x14,0xfc,0x14,0x3f,
    0x8f,0x77,0x79,0xc6,0xeb,0x9f,0x7f,0xa1,0x72,0x99,0xae,0xad,0xb6,0x88,0x90,0x18,
    0x50,0x1d,0x28,0x9e,0x49,0x00,0xf7,0xe4,0x33,0x1b,0x99,0xde,0xc4,0xb5,0x43,0x3a,
    0xc7,0xd3,0x29,0xee,0xb6,0xdd,0x26,0x54,0x5e,0x96,0xe5,0x5b,0x87,0x4b,0xe9,0x09 };

static const uint8_t V_1M[64] = {
    0xe7,0x18,0x48,0x3d,0x0c,0xe7,0x69,0x64,0x4e,0x2e,0x42,0xc7,0xbc,0x15,0xb4,0x63,
    0x8e,0x1f,0x98,0xb1,0x3b,0x20,0x44,0x28,0x56,0x32,0xa8,0x03,0xaf,0xa9,0x73,0xeb,
    0xde,0x0f,0xf2,0x44,0x87,0x7e,0xa6,0x0a,0x4c,0xb0,0x43,0x2c,0xe5,0x77,0xc3,0x1b,
    0xeb,0x00,0x9c,0x5c,0x2c,0x49,0xaa,0x2e,0x4e,0xad,0xb2,0x17,0xad,0x8c,0xc0,0x9b };

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    uint8_t d[SHA512_DIGEST_LEN];
    int ok = 1;

    sha512("", 0, d);
    ok &= check("empty", d, V_EMPTY);

    sha512("abc", 3, d);
    ok &= check("abc", d, V_ABC);

    sha512(MSG112, 112, d);
    ok &= check("len112", d, V_112);

    /* Streamed, deliberately non-block-aligned. No allocation: a fixed 1000-byte
     * stack buffer reused 1000 times. */
    {
        sha512_ctx c;
        uint8_t buf[1000];
        for (unsigned i = 0; i < 1000u; i++)
            buf[i] = 'a';
        sha512_init(&c);
        for (unsigned i = 0; i < 1000u; i++)
            sha512_update(&c, buf, 1000);
        sha512_final(&c, d);
        ok &= check("1M_streamed", d, V_1M);
    }

    if (!ok) {
        wr("SHA512 FAIL\n");
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }
    wr("PRADYOS_SHA512_VECTORS_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
