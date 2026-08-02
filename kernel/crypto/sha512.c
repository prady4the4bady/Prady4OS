/* kernel/crypto/sha512.c — SHA-512 (FIPS 180-4), DDR-821.
 *
 * Straight transcription of the spec. No data-dependent branches and no table
 * indexing by message content, so it is constant-time with respect to its input
 * as a property of the algorithm — which matters here because Ed25519 hashes
 * the private prefix to derive its nonce.
 */
#include "sha512.h"

static inline uint64_t rotr64(uint64_t x, unsigned n) {
    return (x >> n) | (x << (64u - n));
}

/* The four SHA-512 rotation triples. These differ from SHA-256's in every
 * position — 1/8/7 vs 7/18/3, and so on. A copy of the SHA-256 file with the
 * words widened would compile, run, and produce confident wrong digests. */
#define Sig0(x) (rotr64(x, 28) ^ rotr64(x, 34) ^ rotr64(x, 39))
#define Sig1(x) (rotr64(x, 14) ^ rotr64(x, 18) ^ rotr64(x, 41))
#define sig0(x) (rotr64(x,  1) ^ rotr64(x,  8) ^ ((x) >>  7))
#define sig1(x) (rotr64(x, 19) ^ rotr64(x, 61) ^ ((x) >>  6))

#define Ch(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define Maj(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

/* First 64 bits of the fractional parts of the cube roots of the first 80
 * primes. 80 constants, not SHA-256's 64. */
static const uint64_t K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static void sha512_block(sha512_ctx *c, const uint8_t *p) {
    uint64_t w[80];
    for (unsigned i = 0; i < 16u; i++) {
        uint64_t v = 0;
        for (unsigned j = 0; j < 8u; j++)          /* big-endian, per FIPS */
            v = (v << 8) | p[i * 8 + j];
        w[i] = v;
    }
    for (unsigned i = 16u; i < 80u; i++)
        w[i] = sig1(w[i - 2]) + w[i - 7] + sig0(w[i - 15]) + w[i - 16];

    uint64_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint64_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];

    for (unsigned i = 0; i < 80u; i++) {           /* 80 rounds, not 64 */
        uint64_t t1 = h + Sig1(e) + Ch(e, f, g) + K[i] + w[i];
        uint64_t t2 = Sig0(a) + Maj(a, b, cc);
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g;  c->h[7] += h;
}

void sha512_init(sha512_ctx *c) {
    /* First 64 bits of the fractional parts of the square roots of the first
     * eight primes. Distinct from SHA-384, which uses the 9th-16th primes over
     * the same algorithm — another reason this is not a parameterised file. */
    c->h[0] = 0x6a09e667f3bcc908ULL; c->h[1] = 0xbb67ae8584caa73bULL;
    c->h[2] = 0x3c6ef372fe94f82bULL; c->h[3] = 0xa54ff53a5f1d36f1ULL;
    c->h[4] = 0x510e527fade682d1ULL; c->h[5] = 0x9b05688c2b3e6c1fULL;
    c->h[6] = 0x1f83d9abfb41bd6bULL; c->h[7] = 0x5be0cd19137e2179ULL;
    c->len_lo = 0;
    c->len_hi = 0;
    c->buflen = 0;
}

void sha512_update(sha512_ctx *c, const void *data, uint64_t len) {
    const uint8_t *p = (const uint8_t *)data;

    /* 128-bit length counter. The carry is what makes the >2^64-byte case
     * wrong-loudly rather than wrong-silently; it is unreachable in practice
     * and costs one compare. */
    uint64_t before = c->len_lo;
    c->len_lo += len;
    if (c->len_lo < before)
        c->len_hi++;

    while (len > 0) {
        uint32_t space = SHA512_BLOCK_LEN - c->buflen;
        uint32_t take  = (len < (uint64_t)space) ? (uint32_t)len : space;
        for (uint32_t i = 0; i < take; i++)
            c->buf[c->buflen + i] = p[i];
        c->buflen += take;
        p         += take;
        len       -= take;
        if (c->buflen == SHA512_BLOCK_LEN) {
            sha512_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

void sha512_final(sha512_ctx *c, uint8_t out[SHA512_DIGEST_LEN]) {
    /* Length in BITS, as a 128-bit big-endian field. Captured before padding,
     * which is the ordering mistake that produces a plausible wrong digest. */
    uint64_t bits_lo = c->len_lo << 3;
    uint64_t bits_hi = (c->len_hi << 3) | (c->len_lo >> 61);

    uint8_t pad = 0x80u;
    sha512_update(c, &pad, 1);
    /* sha512_update just advanced the length counter; that is harmless because
     * bits_lo/bits_hi were taken above. */
    while (c->buflen != SHA512_BLOCK_LEN - 16u) {
        uint8_t z = 0;
        sha512_update(c, &z, 1);
    }

    uint8_t lenbuf[16];
    for (unsigned i = 0; i < 8u; i++) {
        lenbuf[i]     = (uint8_t)(bits_hi >> (56u - i * 8u));
        lenbuf[8 + i] = (uint8_t)(bits_lo >> (56u - i * 8u));
    }
    sha512_update(c, lenbuf, 16);

    for (unsigned i = 0; i < 8u; i++)
        for (unsigned j = 0; j < 8u; j++)
            out[i * 8 + j] = (uint8_t)(c->h[i] >> (56u - j * 8u));
}

void sha512(const void *data, uint64_t len, uint8_t out[SHA512_DIGEST_LEN]) {
    sha512_ctx c;
    sha512_init(&c);
    sha512_update(&c, data, len);
    sha512_final(&c, out);
}
