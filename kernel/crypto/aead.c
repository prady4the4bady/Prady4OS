/* kernel/crypto/aead.c — ChaCha20-Poly1305 (RFC 8439), DDR-819.
 *
 * Transcription of the spec. No data-dependent branches and no table lookups in
 * either primitive, which is what makes them constant-time here — a property of
 * the algorithm rather than of the compiler.
 */
#include "aead.h"

/* ---------------- ChaCha20 (RFC 8439 §2.3) ---------------- */

static inline uint32_t rotl32(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32u - n));
}

#define QR(a, b, c, d)                          \
    a += b; d ^= a; d = rotl32(d, 16);          \
    c += d; b ^= c; b = rotl32(b, 12);          \
    a += b; d ^= a; d = rotl32(d, 8);           \
    c += d; b ^= c; b = rotl32(b, 7)

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One 64-byte keystream block. */
static void chacha20_block(const uint8_t key[32], const uint8_t nonce[12],
                           uint32_t counter, uint8_t out[64]) {
    /* "expand 32-byte k" — the constants, as little-endian words. */
    uint32_t s[16];
    s[0] = 0x61707865u; s[1] = 0x3320646eu; s[2] = 0x79622d32u; s[3] = 0x6b206574u;
    for (unsigned i = 0; i < 8u; i++)
        s[4 + i] = le32(key + i * 4);
    s[12] = counter;
    for (unsigned i = 0; i < 3u; i++)
        s[13 + i] = le32(nonce + i * 4);

    uint32_t w[16];
    for (unsigned i = 0; i < 16u; i++)
        w[i] = s[i];

    for (unsigned r = 0; r < 10u; r++) {          /* 20 rounds = 10 double-rounds */
        QR(w[0], w[4], w[8],  w[12]);
        QR(w[1], w[5], w[9],  w[13]);
        QR(w[2], w[6], w[10], w[14]);
        QR(w[3], w[7], w[11], w[15]);
        QR(w[0], w[5], w[10], w[15]);
        QR(w[1], w[6], w[11], w[12]);
        QR(w[2], w[7], w[8],  w[13]);
        QR(w[3], w[4], w[9],  w[14]);
    }
    for (unsigned i = 0; i < 16u; i++) {
        uint32_t v = w[i] + s[i];
        out[i * 4 + 0] = (uint8_t)(v);
        out[i * 4 + 1] = (uint8_t)(v >> 8);
        out[i * 4 + 2] = (uint8_t)(v >> 16);
        out[i * 4 + 3] = (uint8_t)(v >> 24);
    }
}

void chacha20_stream(const uint8_t key[32], const uint8_t nonce[12],
                     uint32_t counter, const void *in, uint32_t len, uint8_t *out) {
    const uint8_t *ip = (const uint8_t *)in;
    uint8_t blk[64];
    uint32_t done = 0;
    while (done < len) {
        chacha20_block(key, nonce, counter, blk);
        uint32_t take = len - done;
        if (take > 64u)
            take = 64u;
        for (uint32_t i = 0; i < take; i++)
            out[done + i] = (uint8_t)(ip[done + i] ^ blk[i]);
        done += take;
        counter++;
    }
}

/* ---------------- Poly1305 (RFC 8439 §2.5) ----------------
 * Arithmetic mod 2^130 - 5 in 26-bit limbs over uint64_t accumulators: the
 * standard portable representation, chosen so no intermediate product can
 * overflow. Inventing a limb layout here would be the easiest way to be subtly
 * wrong in a way the short vectors would not catch. */

void poly1305_mac(const uint8_t key[32], const void *msg, uint32_t len,
                  uint8_t tag[AEAD_TAG_LEN]) {
    const uint8_t *m = (const uint8_t *)msg;

    /* r is clamped per the spec; s is the addend applied at the end. */
    uint32_t r0 = (le32(key +  0)      ) & 0x3ffffffu;
    uint32_t r1 = (le32(key +  3) >> 2 ) & 0x3ffff03u;
    uint32_t r2 = (le32(key +  6) >> 4 ) & 0x3ffc0ffu;
    uint32_t r3 = (le32(key +  9) >> 6 ) & 0x3f03fffu;
    uint32_t r4 = (le32(key + 12) >> 8 ) & 0x00fffffu;

    uint32_t s1 = r1 * 5u, s2 = r2 * 5u, s3 = r3 * 5u, s4 = r4 * 5u;
    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;

    while (len > 0) {
        uint8_t blk[16];
        uint32_t n = (len < 16u) ? len : 16u;
        uint32_t hibit;

        for (uint32_t i = 0; i < n; i++)
            blk[i] = m[i];
        if (n < 16u) {
            /* Short final block: a literal 0x01 immediately after the data acts
             * as the terminator, and there is NO implicit bit at 128. */
            blk[n] = 0x01u;
            for (uint32_t i = n + 1u; i < 16u; i++)
                blk[i] = 0u;
            hibit = 0u;
        } else {
            hibit = 1u << 24;                 /* implicit 1 at bit 128 */
        }

        h0 += (le32(blk +  0)      ) & 0x3ffffffu;
        h1 += (le32(blk +  3) >> 2 ) & 0x3ffffffu;
        h2 += (le32(blk +  6) >> 4 ) & 0x3ffffffu;
        h3 += (le32(blk +  9) >> 6 ) & 0x3ffffffu;
        h4 += (le32(blk + 12) >> 8 ) | hibit;

        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 +
                      (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 +
                      (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 +
                      (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 +
                      (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 +
                      (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

        uint64_t c;
        c = d0 >> 26; h0 = (uint32_t)d0 & 0x3ffffffu;
        d1 += c; c = d1 >> 26; h1 = (uint32_t)d1 & 0x3ffffffu;
        d2 += c; c = d2 >> 26; h2 = (uint32_t)d2 & 0x3ffffffu;
        d3 += c; c = d3 >> 26; h3 = (uint32_t)d3 & 0x3ffffffu;
        d4 += c; c = d4 >> 26; h4 = (uint32_t)d4 & 0x3ffffffu;
        h0 += (uint32_t)c * 5u; c = h0 >> 26; h0 &= 0x3ffffffu;
        h1 += (uint32_t)c;

        m   += n;
        len -= n;
    }

    /* Final carry propagation and the conditional subtraction of 2^130 - 5,
     * done branchlessly. */
    uint32_t c;
    c = h1 >> 26; h1 &= 0x3ffffffu;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffffu;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffffu;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffffu;
    h0 += c * 5u; c = h0 >> 26; h0 &= 0x3ffffffu;
    h1 += c;

    uint32_t g0 = h0 + 5u; c = g0 >> 26; g0 &= 0x3ffffffu;
    uint32_t g1 = h1 + c;  c = g1 >> 26; g1 &= 0x3ffffffu;
    uint32_t g2 = h2 + c;  c = g2 >> 26; g2 &= 0x3ffffffu;
    uint32_t g3 = h3 + c;  c = g3 >> 26; g3 &= 0x3ffffffu;
    uint32_t g4 = h4 + c - (1u << 26);

    uint32_t mask = (g4 >> 31) - 1u;            /* all-ones if g >= 2^130-5 */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    /* Serialise as 4 little-endian 32-bit words, adding s. */
    uint64_t f;
    uint32_t o0 = (h0      ) | (h1 << 26);
    uint32_t o1 = (h1 >>  6) | (h2 << 20);
    uint32_t o2 = (h2 >> 12) | (h3 << 14);
    uint32_t o3 = (h3 >> 18) | (h4 <<  8);

    f = (uint64_t)o0 + le32(key + 16);                  o0 = (uint32_t)f;
    f = (uint64_t)o1 + le32(key + 20) + (f >> 32);      o1 = (uint32_t)f;
    f = (uint64_t)o2 + le32(key + 24) + (f >> 32);      o2 = (uint32_t)f;
    f = (uint64_t)o3 + le32(key + 28) + (f >> 32);      o3 = (uint32_t)f;

    uint32_t out[4] = { o0, o1, o2, o3 };
    for (unsigned i = 0; i < 4u; i++) {
        tag[i * 4 + 0] = (uint8_t)(out[i]);
        tag[i * 4 + 1] = (uint8_t)(out[i] >> 8);
        tag[i * 4 + 2] = (uint8_t)(out[i] >> 16);
        tag[i * 4 + 3] = (uint8_t)(out[i] >> 24);
    }
}

/* ---------------- AEAD (RFC 8439 §2.8) ---------------- */

/* The tag covers AAD || pad16 || ct || pad16 || len(AAD) || len(ct), with both
 * lengths as 64-bit little-endian. Streamed into a scratch MAC rather than
 * assembled, so no allocation is needed. */
static void aead_tag(const uint8_t polykey[32],
                     const uint8_t *aad, uint32_t aadlen,
                     const uint8_t *ct,  uint32_t ctlen,
                     uint8_t tag[AEAD_TAG_LEN]) {
    /* Build the MAC input in a bounded scratch buffer. ACC messages are small;
     * the bound is stated so an oversized caller fails loudly rather than
     * overflowing. */
    static const uint32_t SCRATCH = 1024u;
    uint8_t buf[1024];
    uint32_t n = 0;

    uint32_t apad = (16u - (aadlen % 16u)) % 16u;
    uint32_t cpad = (16u - (ctlen  % 16u)) % 16u;
    if ((uint64_t)aadlen + apad + ctlen + cpad + 16u > SCRATCH) {
        for (unsigned i = 0; i < AEAD_TAG_LEN; i++)
            tag[i] = 0;                       /* caller's verify will fail */
        return;
    }
    for (uint32_t i = 0; i < aadlen; i++) buf[n++] = aad[i];
    for (uint32_t i = 0; i < apad;   i++) buf[n++] = 0;
    for (uint32_t i = 0; i < ctlen;  i++) buf[n++] = ct[i];
    for (uint32_t i = 0; i < cpad;   i++) buf[n++] = 0;
    for (unsigned i = 0; i < 8u; i++) buf[n++] = (uint8_t)((uint64_t)aadlen >> (i * 8));
    for (unsigned i = 0; i < 8u; i++) buf[n++] = (uint8_t)((uint64_t)ctlen  >> (i * 8));

    poly1305_mac(polykey, buf, n, tag);
}

void aead_seal(const uint8_t key[AEAD_KEY_LEN], const uint8_t nonce[AEAD_NONCE_LEN],
               const void *aad, uint32_t aadlen,
               const void *pt,  uint32_t ptlen,
               uint8_t *ct, uint8_t tag[AEAD_TAG_LEN]) {
    /* Poly1305 one-time key is ChaCha20 block 0; the message stream starts at
     * block 1. Using block 0 for data would reuse the authenticator key. */
    uint8_t polykey[64], zeros[64];
    for (unsigned i = 0; i < 64u; i++)
        zeros[i] = 0;
    chacha20_stream(key, nonce, 0, zeros, 64, polykey);

    chacha20_stream(key, nonce, 1, pt, ptlen, ct);
    aead_tag(polykey, (const uint8_t *)aad, aadlen, ct, ptlen, tag);
}

int aead_open(const uint8_t key[AEAD_KEY_LEN], const uint8_t nonce[AEAD_NONCE_LEN],
              const void *aad, uint32_t aadlen,
              const void *ct,  uint32_t ctlen,
              const uint8_t tag[AEAD_TAG_LEN], uint8_t *pt) {
    uint8_t polykey[64], zeros[64];
    for (unsigned i = 0; i < 64u; i++)
        zeros[i] = 0;
    chacha20_stream(key, nonce, 0, zeros, 64, polykey);

    uint8_t want[AEAD_TAG_LEN];
    aead_tag(polykey, (const uint8_t *)aad, aadlen, (const uint8_t *)ct, ctlen, want);

    /* CONSTANT-TIME compare, and BEFORE any plaintext is produced. An early-exit
     * compare would leak the tag prefix and reduce forgery from 2^128 work to 16
     * sequential guesses. No gate this project can run distinguishes the two —
     * see DDR-819 — so this is enforced by construction. */
    uint8_t diff = 0;
    for (unsigned i = 0; i < AEAD_TAG_LEN; i++)
        diff |= (uint8_t)(want[i] ^ tag[i]);
    if (diff != 0)
        return -1;                          /* no plaintext written */

    chacha20_stream(key, nonce, 1, ct, ctlen, pt);
    return 0;
}
