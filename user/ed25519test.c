/* user/ed25519test.c — DDR-821 Ed25519 vector probe (RFC 8032 §7.1).
 *
 * Links kernel/crypto/ed25519.c + fe25519.c + sha512.c — the same sources the
 * kernel builds. A probe that reimplemented the group law would test the probe.
 *
 * Checks, each reaching something the others cannot:
 *
 *   1. TEST 1 pubkey+sig — EMPTY message. The length-0 path, which no other
 *      vector reaches and which is easy to get wrong in SHA-512 padding.
 *   2. TEST 2 pubkey+sig — 1-byte message, a second independent key pair.
 *   3. TEST 3 pubkey+sig — 2-byte message, a third independent pair.
 *   4. tampered signature MUST be rejected — the property the primitive exists
 *      for. A verify() that returns 0 unconditionally passes 1-3 completely.
 *   5. wrong public key MUST be rejected — catches a verify() that checks the
 *      signature's internal consistency but never binds it to A.
 *   6. non-canonical S (S >= L) MUST be rejected — signature malleability. A
 *      verify() that skips this accepts a second valid signature for the same
 *      message, which breaks any protocol using the signature as an identifier.
 *   7. long-message round-trip — forces SHA-512 past one block.
 *
 * DEVIATION FROM DDR-821, STATED RATHER THAN QUIETLY DROPPED: the DDR listed
 * RFC 8032 TEST 1024 (a 1023-byte message with a published signature). Check 7
 * uses a GENERATED 1023-byte message and a sign-then-verify round-trip instead.
 * That still exercises the multi-block hash path, but it does NOT compare
 * against a published constant, so it is weaker than TEST 1024 would have been.
 * The reason is provenance, not convenience: TEST 1024's message and signature
 * are 1087 bytes of recalled data, and this file's whole discipline (see
 * ed25519.c's derived curve constants) is to avoid depending on large recalled
 * blobs. SHA-512's own multi-block path is separately gated by smoke-sha512's
 * 1M-'a' streamed vector.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */
#include "ed25519.h"

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
static int cmp(const char *name, const uint8_t *g, const uint8_t *w, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if (g[i] != w[i]) {
            wr("PRADYOS_ED25519_STUB case="); wr(name);
            wr(" first_bad_byte="); wrdec(i); wr("\n");
            return 0;
        }
    }
    return 1;
}

/* RFC 8032 §7.1 TEST 1 — empty message */
static const uint8_t T1_SEED[32] = {
    0x9d,0x61,0xb1,0x9d,0xef,0xfd,0x5a,0x60,0xba,0x84,0x4a,0xf4,0x92,0xec,0x2c,0xc4,
    0x44,0x49,0xc5,0x69,0x7b,0x32,0x69,0x19,0x70,0x3b,0xac,0x03,0x1c,0xae,0x7f,0x60 };
static const uint8_t T1_PUB[32] = {
    0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,
    0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a };
static const uint8_t T1_SIG[64] = {
    0xe5,0x56,0x43,0x00,0xc3,0x60,0xac,0x72,0x90,0x86,0xe2,0xcc,0x80,0x6e,0x82,0x8a,
    0x84,0x87,0x7f,0x1e,0xb8,0xe5,0xd9,0x74,0xd8,0x73,0xe0,0x65,0x22,0x49,0x01,0x55,
    0x5f,0xb8,0x82,0x15,0x90,0xa3,0x3b,0xac,0xc6,0x1e,0x39,0x70,0x1c,0xf9,0xb4,0x6b,
    0xd2,0x5b,0xf5,0xf0,0x59,0x5b,0xbe,0x24,0x65,0x51,0x41,0x43,0x8e,0x7a,0x10,0x0b };

/* TEST 2 — 1-byte message 0x72 */
static const uint8_t T2_SEED[32] = {
    0x4c,0xcd,0x08,0x9b,0x28,0xff,0x96,0xda,0x9d,0xb6,0xc3,0x46,0xec,0x11,0x4e,0x0f,
    0x5b,0x8a,0x31,0x9f,0x35,0xab,0xa6,0x24,0xda,0x8c,0xf6,0xed,0x4f,0xb8,0xa6,0xfb };
static const uint8_t T2_PUB[32] = {
    0x3d,0x40,0x17,0xc3,0xe8,0x43,0x89,0x5a,0x92,0xb7,0x0a,0xa7,0x4d,0x1b,0x7e,0xbc,
    0x9c,0x98,0x2c,0xcf,0x2e,0xc4,0x96,0x8c,0xc0,0xcd,0x55,0xf1,0x2a,0xf4,0x66,0x0c };
static const uint8_t T2_SIG[64] = {
    0x92,0xa0,0x09,0xa9,0xf0,0xd4,0xca,0xb8,0x72,0x0e,0x82,0x0b,0x5f,0x64,0x25,0x40,
    0xa2,0xb2,0x7b,0x54,0x16,0x50,0x3f,0x8f,0xb3,0x76,0x22,0x23,0xeb,0xdb,0x69,0xda,
    0x08,0x5a,0xc1,0xe4,0x3e,0x15,0x99,0x6e,0x45,0x8f,0x36,0x13,0xd0,0xf1,0x1d,0x8c,
    0x38,0x7b,0x2e,0xae,0xb4,0x30,0x2a,0xee,0xb0,0x0d,0x29,0x16,0x12,0xbb,0x0c,0x00 };

/* TEST 3 — 2-byte message af 82 */
static const uint8_t T3_SEED[32] = {
    0xc5,0xaa,0x8d,0xf4,0x3f,0x9f,0x83,0x7b,0xed,0xb7,0x44,0x2f,0x31,0xdc,0xb7,0xb1,
    0x66,0xd3,0x85,0x35,0x07,0x6f,0x09,0x4b,0x85,0xce,0x3a,0x2e,0x0b,0x44,0x58,0xf7 };
static const uint8_t T3_PUB[32] = {
    0xfc,0x51,0xcd,0x8e,0x62,0x18,0xa1,0xa3,0x8d,0xa4,0x7e,0xd0,0x02,0x30,0xf0,0x58,
    0x08,0x16,0xed,0x13,0xba,0x33,0x03,0xac,0x5d,0xeb,0x91,0x15,0x48,0x90,0x80,0x25 };
static const uint8_t T3_SIG[64] = {
    0x62,0x91,0xd6,0x57,0xde,0xec,0x24,0x02,0x48,0x27,0xe6,0x9c,0x3a,0xbe,0x01,0xa3,
    0x0c,0xe5,0x48,0xa2,0x84,0x74,0x3a,0x44,0x5e,0x36,0x80,0xd7,0xdb,0x5a,0xc3,0xac,
    0x18,0xff,0x9b,0x53,0x8d,0x16,0xf2,0x90,0xae,0x67,0xf7,0x60,0x98,0x4d,0xc6,0x59,
    0x4a,0x7c,0x15,0xe9,0x71,0x6e,0xd2,0x8d,0xc0,0x27,0xbe,0xce,0xea,0x1e,0xc4,0x0a };

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    uint8_t pub[32], sig[64];
    int ok = 1;

    /* 1 — empty message */
    ed25519_pubkey(pub, T1_SEED);
    ok &= cmp("t1_pub", pub, T1_PUB, 32);
    ed25519_sign(sig, "", 0, T1_SEED);
    ok &= cmp("t1_sig", sig, T1_SIG, 64);
    if (ed25519_verify(sig, "", 0, T1_PUB) != 0) {
        wr("ED25519 FAIL: t1 verify rejected a valid signature\n"); ok = 0;
    }

    /* 2 — 1-byte message */
    {
        const uint8_t m[1] = { 0x72 };
        ed25519_pubkey(pub, T2_SEED);
        ok &= cmp("t2_pub", pub, T2_PUB, 32);
        ed25519_sign(sig, m, 1, T2_SEED);
        ok &= cmp("t2_sig", sig, T2_SIG, 64);
        if (ed25519_verify(sig, m, 1, T2_PUB) != 0) {
            wr("ED25519 FAIL: t2 verify rejected a valid signature\n"); ok = 0;
        }

        /* 4 — tampered signature must be REJECTED */
        sig[10] ^= 0x01u;
        if (ed25519_verify(sig, m, 1, T2_PUB) == 0) {
            wr("PRADYOS_ED25519_STUB case=tampered_sig_accepted\n"); ok = 0;
        }
        sig[10] ^= 0x01u;

        /* 5 — wrong public key must be REJECTED */
        {
            uint8_t bad[32];
            for (unsigned i = 0; i < 32u; i++) bad[i] = T2_PUB[i];
            bad[3] ^= 0x40u;
            if (ed25519_verify(sig, m, 1, bad) == 0) {
                wr("PRADYOS_ED25519_STUB case=wrong_pubkey_accepted\n"); ok = 0;
            }
        }

        /* 6 — non-canonical S (S >= L) must be REJECTED. Setting the top byte
         * of S to 0xff makes S far larger than L; accepting it would mean the
         * scheme is malleable. */
        {
            uint8_t mal[64];
            for (unsigned i = 0; i < 64u; i++) mal[i] = sig[i];
            mal[63] = 0xffu;
            if (ed25519_verify(mal, m, 1, T2_PUB) == 0) {
                wr("PRADYOS_ED25519_STUB case=noncanonical_S_accepted\n"); ok = 0;
            }
        }
    }

    /* 3 — 2-byte message */
    {
        const uint8_t m[2] = { 0xaf, 0x82 };
        ed25519_pubkey(pub, T3_SEED);
        ok &= cmp("t3_pub", pub, T3_PUB, 32);
        ed25519_sign(sig, m, 2, T3_SEED);
        ok &= cmp("t3_sig", sig, T3_SIG, 64);
        if (ed25519_verify(sig, m, 2, T3_PUB) != 0) {
            wr("ED25519 FAIL: t3 verify rejected a valid signature\n"); ok = 0;
        }
    }

    /* 7 — long message, multi-block hash. Depends on no published constant. */
    {
        uint8_t big[1023];
        for (unsigned i = 0; i < 1023u; i++)
            big[i] = (uint8_t)(i * 31u + 7u);
        ed25519_pubkey(pub, T1_SEED);
        ed25519_sign(sig, big, 1023, T1_SEED);
        if (ed25519_verify(sig, big, 1023, pub) != 0) {
            wr("PRADYOS_ED25519_STUB case=long_msg_roundtrip\n"); ok = 0;
        }
    }

    if (!ok) {
        wr("ED25519 FAIL\n");
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }
    wr("PRADYOS_ED25519_VECTORS_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
