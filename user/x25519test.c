/* user/x25519test.c — DDR-820 X25519 vector probe (RFC 7748).
 *
 * Links kernel/crypto/x25519.c — the same source the kernel builds, compiled a
 * second time with the user code model. A probe that reimplemented the ladder
 * would test the probe.
 *
 * Five checks, each reaching something the others cannot:
 *
 *   1. §5.2 vector 1        — the ordinary path
 *   2. §5.2 vector 2        — a second point/scalar pair; one vector can be
 *                             matched by a broken implementation with a lucky
 *                             carry
 *   3. §6.1 a*G and b*G     — the ONLY checks reaching scalarmult_base, and the
 *                             only ones that would catch a wrong basepoint
 *   4. a*(b*G) == b*(a*G)   — the property the primitive exists for, and the
 *                             ONLY check that depends on no published constant
 *   5. u = 0 rejected       — asserts the contributory check actually fires
 *                             rather than being unreachable code
 *
 * Check 4 is load-bearing for a stated reason: the constants below are
 * RECALLED, not fetched (same caveat as DDR-811/818/819). Checks 1-3 compare
 * against them, so a wrong constant fails loudly — but a consistently wrong set
 * could in principle agree with a wrong implementation. Check 4 compares the
 * implementation against itself under the commutativity Diffie-Hellman
 * requires, and cannot be fooled by bad provenance.
 *
 * It earned that during implementation: the first version had the ladder
 * constant 121665 paired with the BB form, which needs (A+2)/4 = 121666. Every
 * constant-comparing check failed and check 4 PASSED — telling us immediately
 * that the arithmetic was a consistent group law with a wrong constant, not
 * broken field arithmetic. That narrowed a 250-line file to one line.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */
#include "x25519.h"

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
    for (unsigned i = 0; i < 32u; i++) {
        if (got[i] != want[i]) {
            wr("PRADYOS_X25519_STUB case=");
            wr(name);
            wr(" first_bad_byte=");
            wrdec(i);
            wr("\n");
            return 0;
        }
    }
    return 1;
}

/* RFC 7748 §5.2 */
static const uint8_t V1_S[32] = {
    0xa5,0x46,0xe3,0x6b,0xf0,0x52,0x7c,0x9d,0x3b,0x16,0x15,0x4b,0x82,0x46,0x5e,0xdd,
    0x62,0x14,0x4c,0x0a,0xc1,0xfc,0x5a,0x18,0x50,0x6a,0x22,0x44,0xba,0x44,0x9a,0xc4 };
static const uint8_t V1_U[32] = {
    0xe6,0xdb,0x68,0x67,0x58,0x30,0x30,0xdb,0x35,0x94,0xc1,0xa4,0x24,0xb1,0x5f,0x7c,
    0x72,0x66,0x24,0xec,0x26,0xb3,0x35,0x3b,0x10,0xa9,0x03,0xa6,0xd0,0xab,0x1c,0x4c };
static const uint8_t V1_K[32] = {
    0xc3,0xda,0x55,0x37,0x9d,0xe9,0xc6,0x90,0x8e,0x94,0xea,0x4d,0xf2,0x8d,0x08,0x4f,
    0x32,0xec,0xcf,0x03,0x49,0x1c,0x71,0xf7,0x54,0xb4,0x07,0x55,0x77,0xa2,0x85,0x52 };

static const uint8_t V2_S[32] = {
    0x4b,0x66,0xe9,0xd4,0xd1,0xb4,0x67,0x3c,0x5a,0xd2,0x26,0x91,0x95,0x7d,0x6a,0xf5,
    0xc1,0x1b,0x64,0x21,0xe0,0xea,0x01,0xd4,0x2c,0xa4,0x16,0x9e,0x79,0x18,0xba,0x0d };
static const uint8_t V2_U[32] = {
    0xe5,0x21,0x0f,0x12,0x78,0x68,0x11,0xd3,0xf4,0xb7,0x95,0x9d,0x05,0x38,0xae,0x2c,
    0x31,0xdb,0xe7,0x10,0x6f,0xc0,0x3c,0x3e,0xfc,0x4c,0xd5,0x49,0xc7,0x15,0xa4,0x93 };
static const uint8_t V2_K[32] = {
    0x95,0xcb,0xde,0x94,0x76,0xe8,0x90,0x7d,0x7a,0xad,0xe4,0x5c,0xb4,0xb8,0x73,0xf8,
    0x8b,0x59,0x5a,0x68,0x79,0x9f,0xa1,0x52,0xe6,0xf8,0xf7,0x64,0x7a,0xac,0x79,0x57 };

/* RFC 7748 §6.1 */
static const uint8_t A_PRIV[32] = {
    0x77,0x07,0x6d,0x0a,0x73,0x18,0xa5,0x7d,0x3c,0x16,0xc1,0x72,0x51,0xb2,0x66,0x45,
    0xdf,0x4c,0x2f,0x87,0xeb,0xc0,0x99,0x2a,0xb1,0x77,0xfb,0xa5,0x1d,0xb9,0x2c,0x2a };
static const uint8_t A_PUB[32] = {
    0x85,0x20,0xf0,0x09,0x89,0x30,0xa7,0x54,0x74,0x8b,0x7d,0xdc,0xb4,0x3e,0xf7,0x5a,
    0x0d,0xbf,0x3a,0x0d,0x26,0x38,0x1a,0xf4,0xeb,0xa4,0xa9,0x8e,0xaa,0x9b,0x4e,0x6a };
static const uint8_t B_PRIV[32] = {
    0x5d,0xab,0x08,0x7e,0x62,0x4a,0x8a,0x4b,0x79,0xe1,0x7f,0x8b,0x83,0x80,0x0e,0xe6,
    0x6f,0x3b,0xb1,0x29,0x26,0x18,0xb6,0xfd,0x1c,0x2f,0x8b,0x27,0xff,0x88,0xe0,0xeb };
static const uint8_t B_PUB[32] = {
    0xde,0x9e,0xdb,0x7d,0x7b,0x7d,0xc1,0xb4,0xd3,0x5b,0x61,0xc2,0xec,0xe4,0x35,0x37,
    0x3f,0x83,0x43,0xc8,0x5b,0x78,0x67,0x4d,0xad,0xfc,0x7e,0x14,0x6f,0x88,0x2b,0x4f };
static const uint8_t SHARED[32] = {
    0x4a,0x5d,0x9d,0x5b,0xa4,0xce,0x2d,0xe1,0x72,0x8e,0x3b,0xf4,0x80,0x35,0x0f,0x25,
    0xe0,0x7e,0x21,0xc9,0x47,0xd1,0x9e,0x33,0x76,0xf0,0x9b,0x3c,0x1e,0x16,0x17,0x42 };

/* force_align_arg_pointer is load-bearing, and every freestanding probe in
 * user/ has the same latent hazard.
 *
 * SysV says a process entry point is entered with RSP 16-byte ALIGNED (it
 * points at argc), and kernel/exec/elf.c does exactly that. But a compiler
 * treats `_start` as an ordinary function, which is entered via `call` with
 * RSP ≡ 8 (mod 16). So _start's own frame is off by 8, every callee inherits
 * it, and the first 16-byte-aligned stack access faults.
 *
 * That is why real libc _start is written in assembly. Here it cost a #GP at
 * `movaps %xmm0,-0x20(%rbp)` inside x25519.c — clang vectorises the 5-limb
 * field loops at -O2, and `movaps` on a misaligned address raises #GP, not #AC.
 *
 * The other probes survive only because their code never emitted an aligned
 * SSE stack access. The kernel is correct here; do not "fix" the entry RSP. */
__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    uint8_t o[32], apub[32], bpub[32], k1[32], k2[32];
    int ok = 1;

    if (x25519_scalarmult(o, V1_S, V1_U) != 0) {
        wr("X25519 FAIL: vector 1 returned error\n"); ok = 0;
    } else {
        ok &= check("v1", o, V1_K);
    }

    if (x25519_scalarmult(o, V2_S, V2_U) != 0) {
        wr("X25519 FAIL: vector 2 returned error\n"); ok = 0;
    } else {
        ok &= check("v2", o, V2_K);
    }

    /* The only checks that reach scalarmult_base. */
    if (x25519_scalarmult_base(apub, A_PRIV) != 0 ||
        x25519_scalarmult_base(bpub, B_PRIV) != 0) {
        wr("X25519 FAIL: scalarmult_base returned error\n"); ok = 0;
    } else {
        ok &= check("apub", apub, A_PUB);
        ok &= check("bpub", bpub, B_PUB);
    }

    if (x25519_scalarmult(k1, A_PRIV, bpub) != 0 ||
        x25519_scalarmult(k2, B_PRIV, apub) != 0) {
        wr("X25519 FAIL: agreement returned error\n"); ok = 0;
    } else {
        ok &= check("shared", k1, SHARED);
        /* Depends on no published constant. */
        ok &= check("commutativity", k1, k2);
    }

    /* u = 0 is a small-order point: the peer would be CHOOSING the shared
     * secret, not agreeing to it. Must be refused, not returned. */
    {
        uint8_t zero_u[32];
        for (unsigned i = 0; i < 32u; i++) zero_u[i] = 0;
        if (x25519_scalarmult(o, A_PRIV, zero_u) >= 0) {
            wr("PRADYOS_X25519_STUB case=small_order_not_rejected\n");
            ok = 0;
        }
    }

    if (!ok) {
        wr("X25519 FAIL\n");
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }
    wr("PRADYOS_X25519_VECTORS_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
