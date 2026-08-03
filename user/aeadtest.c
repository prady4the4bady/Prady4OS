/* user/aeadtest.c — DDR-819 ChaCha20-Poly1305 vector probe (RFC 8439).
 *
 * Closes a gap flagged repeatedly since DDR-819 landed: aead.{c,h} was verified
 * on the HOST under gcc and had no in-QEMU gate and no build wiring at all.
 * DDR-813 (ACC) is its first real caller and must not consume an ungated
 * primitive.
 *
 * Links kernel/crypto/aead.c — the same source the kernel builds, compiled a
 * second time with the user code model. A probe that reimplemented ChaCha20 or
 * Poly1305 would test the probe.
 *
 * Five checks, each reaching something the others cannot:
 *
 *   1. ChaCha20 keystream (§2.4.2)  — isolates the stream cipher from the AEAD
 *                                     wrapper; a failure here is unambiguous.
 *   2. Poly1305 tag (§2.5.2)        — isolates the authenticator. Its message is
 *                                     34 bytes, i.e. a 2-byte final block, which
 *                                     is the ONLY way to reach the short-block
 *                                     path. A 16-byte-multiple message passes
 *                                     against a broken one — that exact defect
 *                                     was shipped and caught during DDR-819.
 *   3. seal → open round-trip       — the composition: counter start, one-time
 *                                     key derivation, pad16, length encoding.
 *                                     Depends on no published constant.
 *   4. tampered ciphertext          — MUST be rejected. The property the AEAD
 *                                     exists for. An aead_open that never checks
 *                                     the tag passes 1-3 completely.
 *   5. tampered tag                 — MUST be rejected, and distinctly from 4:
 *                                     catches an implementation that recomputes
 *                                     the tag from the ciphertext it was handed
 *                                     without comparing against the one supplied.
 *
 * NOT asserted, and stated so nobody reads the green as more than it is:
 * constant-time tag comparison. A constant-time compare and an early-exit
 * memcmp reject exactly the same inputs — the difference is timing on real
 * hardware, and QEMU under TCG does not model it. See DDR-819 §On arm C.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 * force_align_arg_pointer is mandatory and asserted by ci-start-align-check.
 */
#include "aead.h"

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
static int cmp(const char *name, const uint8_t *got, const uint8_t *want, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if (got[i] != want[i]) {
            wr("PRADYOS_AEAD_STUB case="); wr(name);
            wr(" first_bad_byte="); wrdec(i); wr("\n");
            return 0;
        }
    }
    return 1;
}

/* RFC 8439 §2.4.2 — key 00..1f, counter 1. Only the first 16 ciphertext bytes
 * are compared: enough to catch any state/rotation/constant error, and it keeps
 * the probe small.
 *
 * THE NONCE IS ALL-ZEROS THEN 4a, AND THAT IS THE WHOLE POINT OF THIS COMMENT.
 * RFC 8439 carries two different nonces a few pages apart:
 *
 *   §2.3.2 (block function test)  00 00 00 09 | 00 00 00 4a | 00 00 00 00
 *   §2.4.2 (encryption test)      00 00 00 00 | 00 00 00 4a | 00 00 00 00
 *
 * The first version of this probe paired §2.3.2's nonce with §2.4.2's expected
 * ciphertext. A different nonce changes the keystream from byte 0, so the gate
 * failed with `first_bad_byte=0` on its very first run — a gross mismatch that
 * correctly did NOT look like a carry bug. `aead.c` was right the whole time;
 * the recalled constant was wrong. Confirmed by testing BOTH candidate nonces
 * against the same primitive on the host: the §2.4.2 one matches, the §2.3.2
 * one does not. */
static const uint8_t K244[32] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f };
static const uint8_t N244[12] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4a,0x00,0x00,0x00,0x00 };
static const char PT244[] =
    "Ladies and Gentlemen of the class of '99: If I could offer you only one tip "
    "for the future, sunscreen would be it.";
static const uint8_t CT244_16[16] = {
    0x6e,0x2e,0x35,0x9a,0x25,0x68,0xf9,0x80,0x41,0xba,0x07,0x28,0xdd,0x0d,0x69,0x81 };

/* RFC 8439 §2.5.2 — 34-byte message, so the final block is 2 bytes. */
static const uint8_t K252[32] = {
    0x85,0xd6,0xbe,0x78,0x57,0x55,0x6d,0x33,0x7f,0x44,0x52,0xfe,0x42,0xd5,0x06,0xa8,
    0x01,0x03,0x80,0x8a,0xfb,0x0d,0xb2,0xfd,0x4a,0xbf,0xf6,0xaf,0x41,0x49,0xf5,0x1b };
static const char M252[] = "Cryptographic Forum Research Group";   /* 34 bytes */
static const uint8_t T252[16] = {
    0xa8,0x06,0x1d,0xc1,0x30,0x51,0x36,0xc6,0xc2,0x2b,0x8b,0xaf,0x0c,0x01,0x27,0xa9 };

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    int ok = 1;

    /* 1 — ChaCha20 keystream, isolated from the AEAD wrapper. */
    {
        uint8_t out[128];
        unsigned n = 0;
        while (PT244[n]) n++;
        chacha20_stream(K244, N244, 1, PT244, n, out);
        ok &= cmp("chacha20_2_4_2", out, CT244_16, 16);
    }

    /* 2 — Poly1305, isolated from the cipher. 2-byte final block. */
    {
        uint8_t tag[AEAD_TAG_LEN];
        unsigned n = 0;
        while (M252[n]) n++;
        poly1305_mac(K252, M252, n, tag);
        ok &= cmp("poly1305_2_5_2", tag, T252, AEAD_TAG_LEN);
    }

    /* 3 — seal/open round-trip. Depends on no published constant, so it stays
     * meaningful even if every recalled vector above were wrong together. */
    uint8_t ct[128], pt2[128], tag[AEAD_TAG_LEN];
    unsigned ptlen = 0;
    while (PT244[ptlen]) ptlen++;
    {
        const uint8_t aad[4] = { 0xde, 0xad, 0xbe, 0xef };
        aead_seal(K244, N244, aad, 4, PT244, ptlen, ct, tag);
        if (aead_open(K244, N244, aad, 4, ct, ptlen, tag, pt2) != 0) {
            wr("AEAD FAIL: round-trip open rejected a valid box\n");
            ok = 0;
        } else {
            ok &= cmp("roundtrip", pt2, (const uint8_t *)PT244, ptlen);
        }

        /* 4 — one bit flipped in the ciphertext MUST be rejected. */
        ct[0] ^= 0x01u;
        if (aead_open(K244, N244, aad, 4, ct, ptlen, tag, pt2) == 0) {
            wr("PRADYOS_AEAD_STUB case=tampered_ct_accepted\n");
            ok = 0;
        }
        ct[0] ^= 0x01u;                       /* restore */

        /* 5 — one bit flipped in the TAG must also be rejected, and this is a
         * different failure from 4: it catches an open() that recomputes the tag
         * over the ciphertext it was given but never compares it to the tag it
         * was handed. */
        tag[0] ^= 0x01u;
        if (aead_open(K244, N244, aad, 4, ct, ptlen, tag, pt2) == 0) {
            wr("PRADYOS_AEAD_STUB case=tampered_tag_accepted\n");
            ok = 0;
        }
    }

    if (!ok) {
        wr("AEAD FAIL\n");
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }
    wr("PRADYOS_AEAD_VECTORS_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
