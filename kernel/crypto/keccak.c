/* kernel/crypto/keccak.c — Keccak-f[1600] + SHA-3/SHAKE (FIPS 202), DDR-1052.
 * See keccak.h for why this exists and why it is endian-neutral. */
#include "keccak.h"
#include "keccak_kat.h"

/* Round constants and rho offsets are DERIVED, not transcribed. Both were
 * generated from the FIPS 202 definitions and then PROVED by implementing this
 * same permutation in Python against hashlib's SHA-3/SHAKE across four
 * functions and four message lengths (including rate boundaries) before a line
 * of C was written. The first draft of that generator produced RC[0]=0x03
 * instead of 0x01 -- a wrong LFSR index -- which is exactly the transcription
 * class this procedure exists to catch. */
static const uint64_t KECCAK_RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL,
    0x8000000080008000ULL, 0x000000000000808BULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008AULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000AULL,
    0x000000008000808BULL, 0x800000000000008BULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800AULL, 0x800000008000000AULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};

/* rho offsets, indexed [x*5 + y]. */
static const uint8_t KECCAK_RHO[25] = {
     0, 36,  3, 41, 18,
     1, 44, 10, 45,  2,
    62,  6, 43, 15, 61,
    28, 55, 25, 21, 56,
    27, 20, 39,  8, 14,
};

static inline uint64_t rol64(uint64_t v, unsigned n) {
    return n ? ((v << n) | (v >> (64u - n))) : v;
}

/* Lane index is x + 5*y, which is exactly byte_offset/8 for the little-endian
 * byte ordering FIPS 202 specifies -- so absorb/squeeze index the flat array
 * directly with no mapping table. */
static void keccak_f1600(uint64_t a[25]) {
    for (unsigned ir = 0; ir < 24u; ir++) {
        uint64_t c[5], d[5], b[25];
        unsigned x, y;

        for (x = 0; x < 5u; x++)
            c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
        for (x = 0; x < 5u; x++)
            d[x] = c[(x + 4u) % 5u] ^ rol64(c[(x + 1u) % 5u], 1u);
        for (x = 0; x < 5u; x++)
            for (y = 0; y < 5u; y++)
                a[x + 5u * y] ^= d[x];

        for (x = 0; x < 5u; x++)
            for (y = 0; y < 5u; y++)
                b[y + 5u * ((2u * x + 3u * y) % 5u)] =
                    rol64(a[x + 5u * y], KECCAK_RHO[x * 5u + y]);

        for (x = 0; x < 5u; x++)
            for (y = 0; y < 5u; y++)
                a[x + 5u * y] = b[x + 5u * y] ^
                    ((~b[((x + 1u) % 5u) + 5u * y]) & b[((x + 2u) % 5u) + 5u * y]);

        a[0] ^= KECCAK_RC[ir];
    }
}

void keccak_init(keccak_ctx *c, unsigned rate, uint8_t dom) {
    for (unsigned i = 0; i < 25u; i++)
        c->st[i] = 0;
    c->rate = rate;
    c->pos = 0;
    c->dom = dom;
    c->squeezing = 0;
}

void keccak_update(keccak_ctx *c, const uint8_t *in, size_t len) {
    for (size_t i = 0; i < len; i++) {
        c->st[c->pos >> 3] ^= (uint64_t)in[i] << (8u * (c->pos & 7u));
        if (++c->pos == c->rate) {
            keccak_f1600(c->st);
            c->pos = 0;
        }
    }
}

/* pad10*1 with the domain byte folded in, then the high bit of the last rate
 * byte. When pos == rate-1 both land in the SAME byte, which is why they are
 * two XORs into one position rather than a write and a write. */
static void keccak_pad(keccak_ctx *c) {
    c->st[c->pos >> 3] ^= (uint64_t)c->dom << (8u * (c->pos & 7u));
    unsigned last = c->rate - 1u;
    c->st[last >> 3] ^= 0x80ULL << (8u * (last & 7u));
    keccak_f1600(c->st);
    c->pos = 0;
    c->squeezing = 1;
}

void keccak_squeeze(keccak_ctx *c, uint8_t *out, size_t len) {
    if (!c->squeezing)
        keccak_pad(c);
    for (size_t i = 0; i < len; i++) {
        if (c->pos == c->rate) {
            keccak_f1600(c->st);
            c->pos = 0;
        }
        out[i] = (uint8_t)(c->st[c->pos >> 3] >> (8u * (c->pos & 7u)));
        c->pos++;
    }
}

static void xof(unsigned rate, uint8_t dom, const uint8_t *in, size_t inlen,
                uint8_t *out, size_t outlen) {
    keccak_ctx c;
    keccak_init(&c, rate, dom);
    keccak_update(&c, in, inlen);
    keccak_squeeze(&c, out, outlen);
}

void shake128(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen) {
    xof(KECCAK_RATE_SHAKE128, 0x1F, in, inlen, out, outlen);
}
void shake256(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen) {
    xof(KECCAK_RATE_SHAKE256, 0x1F, in, inlen, out, outlen);
}
void sha3_256(const uint8_t *in, size_t inlen, uint8_t out[32]) {
    xof(KECCAK_RATE_SHA3_256, 0x06, in, inlen, out, 32u);
}
void sha3_512(const uint8_t *in, size_t inlen, uint8_t out[64]) {
    xof(KECCAK_RATE_SHA3_512, 0x06, in, inlen, out, 64u);
}

/* ---------------------------------------------------------------------------
 * Known-answer self-test. Returns 0, or the 1-based index of the first failure.
 *
 * Every expected value in keccak_kat.h came from Python's hashlib, i.e. from an
 * implementation that is not this one. Vectors 7-12 are the rate boundaries
 * (SHAKE128 absorbs 168 bytes per permutation, SHAKE256 absorbs 136) and 13-14
 * are squeezes longer than the rate; those are the arms that fail on a wrong
 * implementation which still passes the empty and "abc" cases.
 *
 * Vector 15 is not a KAT: it feeds the SAME message in irregular chunks and
 * requires the one-shot answer back. A one-shot-only vector set cannot catch a
 * broken streaming offset, because it never calls update() twice.
 * -------------------------------------------------------------------------*/
static int kat_eq(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static void kat_pattern(uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)((i * 7u + 3u) & 0xFFu);
}

int keccak_selftest(void) {
    static const uint8_t abc[3] = { 'a', 'b', 'c' };
    uint8_t out[200];
    uint8_t msg[169];
    int n = 0;

    n++; shake128(0, 0, out, 32);
    if (!kat_eq(out, KAT_shake128_empty, 32)) return n;
    n++; shake256(0, 0, out, 32);
    if (!kat_eq(out, KAT_shake256_empty, 32)) return n;
    n++; sha3_256(0, 0, out);
    if (!kat_eq(out, KAT_sha3_256_empty, 32)) return n;
    n++; sha3_512(0, 0, out);
    if (!kat_eq(out, KAT_sha3_512_empty, 64)) return n;
    n++; shake128(abc, 3, out, 32);
    if (!kat_eq(out, KAT_shake128_abc, 32)) return n;
    n++; shake256(abc, 3, out, 32);
    if (!kat_eq(out, KAT_shake256_abc, 32)) return n;

    /* SHAKE128 absorb boundary: rate-1, rate, rate+1. */
    n++; kat_pattern(msg, 167); shake128(msg, 167, out, 32);
    if (!kat_eq(out, KAT_shake128_pat167, 32)) return n;
    n++; kat_pattern(msg, 168); shake128(msg, 168, out, 32);
    if (!kat_eq(out, KAT_shake128_pat168, 32)) return n;
    n++; kat_pattern(msg, 169); shake128(msg, 169, out, 32);
    if (!kat_eq(out, KAT_shake128_pat169, 32)) return n;

    /* SHAKE256 absorb boundary. */
    n++; kat_pattern(msg, 135); shake256(msg, 135, out, 32);
    if (!kat_eq(out, KAT_shake256_pat135, 32)) return n;
    n++; kat_pattern(msg, 136); shake256(msg, 136, out, 32);
    if (!kat_eq(out, KAT_shake256_pat136, 32)) return n;
    n++; kat_pattern(msg, 137); shake256(msg, 137, out, 32);
    if (!kat_eq(out, KAT_shake256_pat137, 32)) return n;

    /* Squeeze past the rate: assert the TAIL, which only a correct second
     * permutation produces. */
    n++; shake128(abc, 3, out, 200);
    if (!kat_eq(out + 168, KAT_shake128_abc200_tail, 32)) return n;
    n++; shake256(abc, 3, out, 200);
    if (!kat_eq(out + 168, KAT_shake256_abc200_tail, 32)) return n;

    /* Streaming: same message, irregular chunk sizes, one-shot answer. */
    n++;
    {
        keccak_ctx c;
        uint8_t s[32];
        kat_pattern(msg, 169);
        shake128_init(&c);
        keccak_update(&c, msg, 1);
        keccak_update(&c, msg + 1, 7);
        keccak_update(&c, msg + 8, 160);
        keccak_update(&c, msg + 168, 1);
        keccak_squeeze(&c, s, 32);
        if (!kat_eq(s, KAT_shake128_pat169, 32)) return n;
    }
    return 0;
}
