/* kernel/crypto/mldsa.c — ML-DSA-44 (FIPS 204) key generation, DDR-1054.
 *
 * Step 2 of Post-Quantum Security, on top of DDR-1052's Keccak core.
 *
 * NO MAGIC CONSTANTS. Everything that could be mistranscribed is DERIVED at
 * run time from the two numbers FIPS 204 actually defines -- q and the root
 * zeta = 1753 -- because DDR-1052 learned this the expensive way: its first
 * Keccak round-constant generator produced RC[0] = 0x03 instead of 0x01, a
 * silent total break of the kind hand-copying 24 magic 64-bit values invites.
 * So the 256 twiddle factors are computed as zeta^brv8(i), and the inverse-NTT
 * scale factor is computed as 256^(q-2) mod q -- which is 8347681, the value
 * the reference carries as a literal.
 *
 * Plain modular arithmetic, no Montgomery form. keyGen runs once per selftest,
 * so ~50k 64-bit modulos is nothing, and clarity is worth more than speed in
 * code whose only feedback is a 1312-byte answer that either matches or does
 * not.
 *
 * The oracle for every stage of this file is tools/ci/mldsa_ref.py, which
 * reproduces NIST ACVP tcId 1-5 byte-exactly (DDR-1053).
 */
#include "mldsa.h"
#include "keccak.h"
#include "mldsa_kat.h"

#define Q          8380417
#define D          13
#define ETA        2
#define ZETA_ROOT  1753

/* NO file-scope mutable state, deliberately: the derived tables live in the
 * caller's scratch. That makes keyGen genuinely reentrant (which mldsa.h always
 * claimed) and, just as importantly, leaves the ring-3 probe with no writable
 * allocated section -- user/user.ld gives each probe a single R+X PT_LOAD, so a
 * `static` here would link fine and fault on its first store (DDR-826). */

static uint32_t modmul(uint32_t a, uint32_t b) {
    return (uint32_t)(((uint64_t)a * (uint64_t)b) % (uint64_t)Q);
}

static uint32_t modpow(uint32_t base, uint32_t exp) {
    uint32_t r = 1, b = base % Q;
    while (exp) {
        if (exp & 1u) r = modmul(r, b);
        b = modmul(b, b);
        exp >>= 1;
    }
    return r;
}

/* 8-bit bit reversal: the NTT's twiddle order. */
static unsigned brv8(unsigned x) {
    unsigned r = 0;
    for (unsigned i = 0; i < 8; i++) r |= ((x >> i) & 1u) << (7u - i);
    return r;
}

static void tables_init(mldsa44_scratch *sc) {
    if (sc->tables_ready) return;
    for (unsigned i = 0; i < 256; i++)
        sc->zetas[i] = modpow(ZETA_ROOT, (uint32_t)brv8(i));
    sc->ninv = modpow(256, (uint32_t)(Q - 2));    /* Fermat inverse */
    sc->tables_ready = 1;
}

/* ---- NTT / inverse NTT over Z_q[X]/(X^256+1) --------------------------- */

static void ntt(const mldsa44_scratch *sc, int32_t w[MLDSA_N]) {
    unsigned m = 0, len = 128;
    for (;;) {
        for (unsigned start = 0; start < MLDSA_N; start += 2u * len) {
            uint32_t z = sc->zetas[++m];
            for (unsigned j = start; j < start + len; j++) {
                uint32_t t = modmul(z, (uint32_t)w[j + len]);
                w[j + len] = (int32_t)(((uint32_t)w[j] + (uint32_t)Q - t) % Q);
                w[j]       = (int32_t)(((uint32_t)w[j] + t) % Q);
            }
        }
        if (len == 1) break;
        len >>= 1;
    }
}

static void intt(const mldsa44_scratch *sc, int32_t w[MLDSA_N]) {
    unsigned m = 256, len = 1;
    while (len < MLDSA_N) {
        for (unsigned start = 0; start < MLDSA_N; start += 2u * len) {
            uint32_t z = (uint32_t)Q - sc->zetas[--m];
            for (unsigned j = start; j < start + len; j++) {
                uint32_t t = (uint32_t)w[j];
                uint32_t u = (uint32_t)w[j + len];
                w[j]       = (int32_t)((t + u) % Q);
                w[j + len] = (int32_t)(modmul(z, (t + (uint32_t)Q - u) % Q));
            }
        }
        len <<= 1;
    }
    for (unsigned j = 0; j < MLDSA_N; j++)
        w[j] = (int32_t)modmul((uint32_t)w[j], sc->ninv);
}

/* ---- sampling ---------------------------------------------------------- */

/* RejNTTPoly (FIPS 204 Alg. 30): SHAKE128(rho || s || r), 23-bit candidates,
 * rejecting anything >= q. STREAMED rather than squeezed into a fixed buffer:
 * the rejection rate is only ~0.1%, so a fixed 2304-byte squeeze would in
 * practice always suffice -- "in practice always" is not a bound, and the
 * streaming form has no failure mode to reason about. */
static void rej_ntt_poly(const uint8_t rho[32], uint8_t s, uint8_t r,
                         int32_t a[MLDSA_N]) {
    keccak_ctx c;
    uint8_t sr[2];
    unsigned n = 0;

    shake128_init(&c);
    keccak_update(&c, rho, 32);
    sr[0] = s; sr[1] = r;
    keccak_update(&c, sr, 2);

    while (n < MLDSA_N) {
        uint8_t b[3];
        uint32_t v;
        keccak_squeeze(&c, b, 3);
        v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | (((uint32_t)b[2] & 0x7Fu) << 16);
        if (v < (uint32_t)Q) a[n++] = (int32_t)v;
    }
}

/* RejBoundedPoly for eta = 2 (FIPS 204 Alg. 31): SHAKE256(rhop || idx), two
 * nibbles per byte, each < 15 mapping to 2 - (z mod 5) in [-2, 2].
 * Stored CENTERED (int8), not mod q: the secret packing wants eta - c, and the
 * two places that need a residue convert on the spot. */
static void rej_bounded_poly(const uint8_t rhop[64], unsigned idx,
                             int8_t a[MLDSA_N]) {
    keccak_ctx c;
    uint8_t ib[2];
    unsigned n = 0;

    shake256_init(&c);
    keccak_update(&c, rhop, 64);
    ib[0] = (uint8_t)(idx & 0xFFu);
    ib[1] = (uint8_t)(idx >> 8);
    keccak_update(&c, ib, 2);

    while (n < MLDSA_N) {
        uint8_t b;
        unsigned z[2];
        keccak_squeeze(&c, &b, 1);
        z[0] = b & 0x0Fu;
        z[1] = (unsigned)(b >> 4);
        for (unsigned k = 0; k < 2u && n < MLDSA_N; k++)
            if (z[k] < 15u)
                a[n++] = (int8_t)(2 - (int)(z[k] % 5u));
    }
}

/* ---- Power2Round + bit packing ----------------------------------------- */

static void power2round(uint32_t r, int32_t *t1, int32_t *t0) {
    int32_t r0 = (int32_t)(r & ((1u << D) - 1u));
    if (r0 > (1 << (D - 1))) r0 -= (1 << D);
    *t0 = r0;
    *t1 = (int32_t)(((int32_t)r - r0) >> D);
}

/* SimpleBitPack: little-endian bit stream, `bits` per coefficient. */
static void bitpack(const int32_t *a, unsigned bits, uint8_t *out) {
    uint32_t acc = 0;
    unsigned nb = 0, o = 0;
    for (unsigned i = 0; i < MLDSA_N; i++) {
        acc |= ((uint32_t)a[i] & ((1u << bits) - 1u)) << nb;
        nb += bits;
        while (nb >= 8u) { out[o++] = (uint8_t)(acc & 0xFFu); acc >>= 8; nb -= 8u; }
    }
    if (nb) out[o] = (uint8_t)(acc & 0xFFu);
}

/* ---- keyGen ------------------------------------------------------------ */

int mldsa44_keygen(const uint8_t seed[MLDSA44_SEED_BYTES],
                   uint8_t pk[MLDSA44_PK_BYTES],
                   uint8_t sk[MLDSA44_SK_BYTES],
                   mldsa44_scratch *sc)
{
    uint8_t h[128], kl[2];
    const uint8_t *rho, *rhop, *kk;
    keccak_ctx c;
    unsigned r, s, j, o;
    int32_t packbuf[MLDSA_N];

    tables_init(sc);

    /* H(seed || k || l) -> rho (32) || rho' (64) || K (32). The k and l bytes
     * are FIPS 204 (Alg. 1) domain separation added in the final standard;
     * omitting them yields a self-consistent WRONG key that only a real ACVP
     * vector can catch, which is why the KAT is byte-exact. */
    shake256_init(&c);
    keccak_update(&c, seed, MLDSA44_SEED_BYTES);
    kl[0] = (uint8_t)MLDSA_K; kl[1] = (uint8_t)MLDSA_L;
    keccak_update(&c, kl, 2);
    keccak_squeeze(&c, h, 128);
    rho = h; rhop = h + 32; kk = h + 96;

    for (s = 0; s < MLDSA_L; s++) rej_bounded_poly(rhop, s, sc->s1[s]);
    for (r = 0; r < MLDSA_K; r++) rej_bounded_poly(rhop, MLDSA_L + r, sc->s2[r]);

    /* NTT(s1), from the centered secrets. */
    for (s = 0; s < MLDSA_L; s++) {
        for (j = 0; j < MLDSA_N; j++) {
            int32_t v = sc->s1[s][j];
            sc->s1hat[s][j] = (v < 0) ? (v + Q) : v;
        }
        ntt(sc, sc->s1hat[s]);
    }

    /* t = A*s1 + s2, one row at a time. A is expanded ON THE FLY -- holding
     * all 16 polynomials would cost 16 KiB for no benefit, since each is used
     * exactly once. */
    for (r = 0; r < MLDSA_K; r++) {
        for (j = 0; j < MLDSA_N; j++) sc->acc[j] = 0;
        for (s = 0; s < MLDSA_L; s++) {
            rej_ntt_poly(rho, (uint8_t)s, (uint8_t)r, sc->aij);
            for (j = 0; j < MLDSA_N; j++)
                sc->acc[j] = (int32_t)(((uint32_t)sc->acc[j]
                              + modmul((uint32_t)sc->aij[j],
                                       (uint32_t)sc->s1hat[s][j])) % Q);
        }
        intt(sc, sc->acc);
        for (j = 0; j < MLDSA_N; j++) {
            int32_t v = sc->s2[r][j];
            uint32_t t = ((uint32_t)sc->acc[j] + (uint32_t)((v < 0) ? (v + Q) : v)) % Q;
            power2round(t, &sc->t1[r][j], &sc->t0[r][j]);
        }
    }

    /* pk = rho || SimpleBitPack(t1, 10 bits) */
    for (j = 0; j < 32u; j++) pk[j] = rho[j];
    o = 32;
    for (r = 0; r < MLDSA_K; r++) { bitpack(sc->t1[r], 10, pk + o); o += 320; }

    /* sk = rho || K || tr || s1 || s2 || t0, with tr = H(pk, 64). */
    for (j = 0; j < 32u; j++) sk[j] = rho[j];
    for (j = 0; j < 32u; j++) sk[32 + j] = kk[j];
    shake256(pk, MLDSA44_PK_BYTES, sk + 64, 64);
    o = 128;
    for (s = 0; s < MLDSA_L; s++) {
        for (j = 0; j < MLDSA_N; j++) packbuf[j] = ETA - sc->s1[s][j];
        bitpack(packbuf, 3, sk + o); o += 96;
    }
    for (r = 0; r < MLDSA_K; r++) {
        for (j = 0; j < MLDSA_N; j++) packbuf[j] = ETA - sc->s2[r][j];
        bitpack(packbuf, 3, sk + o); o += 96;
    }
    for (r = 0; r < MLDSA_K; r++) {
        for (j = 0; j < MLDSA_N; j++) packbuf[j] = (1 << (D - 1)) - sc->t0[r][j];
        bitpack(packbuf, 13, sk + o); o += 416;
    }
    return 0;
}

/* mldsa_kat.h is GENERATED and emits one array per field per vector, so the
 * table is assembled here rather than there. The static assert is what keeps
 * that honest: regenerating the header with more vectors would otherwise leave
 * this file silently testing only the first two, which is the dead-arm class --
 * a check whose coverage quietly shrinks while the gate stays green. */
_Static_assert(MLDSA44_KAT_COUNT == 2,
               "mldsa_kat.h gained or lost vectors; extend the table below");

static const struct {
    const uint8_t *seed, *pk, *sk;
} MLDSA44_KATS[MLDSA44_KAT_COUNT] = {
    { MLDSA44_SEED_1, MLDSA44_PK_1, MLDSA44_SK_1 },
    { MLDSA44_SEED_2, MLDSA44_PK_2, MLDSA44_SK_2 },
};

/* Power2Round boundary arm. THE KATs DO NOT COVER THIS, and that was measured
 * rather than assumed: r0 == 2^(D-1) exactly occurs in 0 of the 2048
 * coefficients across both pinned vectors, and the expected rate is ~0.125 hits
 * per key -- so a mutant flipping this comparison from `>` to `>=` passes the
 * KAT arm outright (DDR-1054 M3). These ten cases are generated from the FIPS
 * 204 definition and bracket the boundary from both sides, which is what makes
 * that mutant fail. */
static const struct { uint32_t r; int32_t t1, t0; } P2R_VEC[] = {
    {        0u,    0,     0 },
    {        1u,    0,     1 },
    {     4095u,    0,  4095 },
    {     4096u,    0,  4096 },   /* the boundary itself: r0 == 2^(D-1)      */
    {     4097u,    1, -4095 },   /* one past it, where the branch DOES fire */
    {     8191u,    1,    -1 },
    {     8192u,    1,     0 },
    {     8193u,    1,     1 },
    {    12288u,    1,  4096 },   /* the boundary again, one limb up         */
    {  8380416u, 1023,     0 },   /* q-1                                     */
};

static int power2round_selftest(void) {
    for (unsigned i = 0; i < sizeof P2R_VEC / sizeof P2R_VEC[0]; i++) {
        int32_t t1 = 0, t0 = 0;
        power2round(P2R_VEC[i].r, &t1, &t0);
        if (t1 != P2R_VEC[i].t1 || t0 != P2R_VEC[i].t0)
            return (int)i + 1;
    }
    return 0;
}

unsigned mldsa44_kat_count(void) { return MLDSA44_KAT_COUNT; }
unsigned mldsa44_p2r_count(void) { return (unsigned)(sizeof P2R_VEC / sizeof P2R_VEC[0]); }

int mldsa44_selftest(mldsa44_scratch *sc) {
    uint8_t *pk = sc->pk, *sk = sc->sk;

    /* Boundary arm first: it is deterministic and costs nothing, and a failure
     * here localises the defect far better than a 1312-byte mismatch does.
     * Reported as a NEGATIVE index so the two arms cannot be confused. */
    { int p = power2round_selftest(); if (p) return -p; }

    for (unsigned v = 0; v < MLDSA44_KAT_COUNT; v++) {
        mldsa44_keygen(MLDSA44_KATS[v].seed, pk, sk, sc);
        for (unsigned i = 0; i < MLDSA44_PK_BYTES; i++)
            if (pk[i] != MLDSA44_KATS[v].pk[i]) return (int)v + 1;
        for (unsigned i = 0; i < MLDSA44_SK_BYTES; i++)
            if (sk[i] != MLDSA44_KATS[v].sk[i]) return (int)v + 1;
    }
    return 0;
}
