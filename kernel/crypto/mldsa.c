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

static void tables_fill(uint32_t *zetas, uint32_t *ninv) {
    for (unsigned i = 0; i < 256; i++)
        zetas[i] = modpow(ZETA_ROOT, (uint32_t)brv8(i));
    *ninv = modpow(256, (uint32_t)(Q - 2));      /* Fermat inverse */
}

static void tables_init(mldsa44_scratch *sc) {
    if (sc->tables_ready) return;
    tables_fill(sc->zetas, &sc->ninv);
    sc->tables_ready = 1;
}

/* ---- NTT / inverse NTT over Z_q[X]/(X^256+1) --------------------------- */

static void ntt(const uint32_t *zetas, int32_t w[MLDSA_N]) {
    unsigned m = 0, len = 128;
    for (;;) {
        for (unsigned start = 0; start < MLDSA_N; start += 2u * len) {
            uint32_t z = zetas[++m];
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

static void intt(const uint32_t *zetas, uint32_t ninv, int32_t w[MLDSA_N]) {
    unsigned m = 256, len = 1;
    while (len < MLDSA_N) {
        for (unsigned start = 0; start < MLDSA_N; start += 2u * len) {
            uint32_t z = (uint32_t)Q - zetas[--m];
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
        w[j] = (int32_t)modmul((uint32_t)w[j], ninv);
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
static void bitpack_simple(const int32_t *a, unsigned bits, uint8_t *out) {
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
        ntt(sc->zetas, sc->s1hat[s]);
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
        intt(sc->zetas, sc->ninv, sc->acc);
        for (j = 0; j < MLDSA_N; j++) {
            int32_t v = sc->s2[r][j];
            uint32_t t = ((uint32_t)sc->acc[j] + (uint32_t)((v < 0) ? (v + Q) : v)) % Q;
            power2round(t, &sc->t1[r][j], &sc->t0[r][j]);
        }
    }

    /* pk = rho || SimpleBitPack(t1, 10 bits) */
    for (j = 0; j < 32u; j++) pk[j] = rho[j];
    o = 32;
    for (r = 0; r < MLDSA_K; r++) { bitpack_simple(sc->t1[r], 10, pk + o); o += 320; }

    /* sk = rho || K || tr || s1 || s2 || t0, with tr = H(pk, 64). */
    for (j = 0; j < 32u; j++) sk[j] = rho[j];
    for (j = 0; j < 32u; j++) sk[32 + j] = kk[j];
    shake256(pk, MLDSA44_PK_BYTES, sk + 64, 64);
    o = 128;
    for (s = 0; s < MLDSA_L; s++) {
        for (j = 0; j < MLDSA_N; j++) packbuf[j] = ETA - sc->s1[s][j];
        bitpack_simple(packbuf, 3, sk + o); o += 96;
    }
    for (r = 0; r < MLDSA_K; r++) {
        for (j = 0; j < MLDSA_N; j++) packbuf[j] = ETA - sc->s2[r][j];
        bitpack_simple(packbuf, 3, sk + o); o += 96;
    }
    for (r = 0; r < MLDSA_K; r++) {
        for (j = 0; j < MLDSA_N; j++) packbuf[j] = (1 << (D - 1)) - sc->t0[r][j];
        bitpack_simple(packbuf, 13, sk + o); o += 416;
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

/* ========================================================================
 * DDR-1057 — ML-DSA.Sign_internal (FIPS 204 Alg. 7), deterministic variant.
 *
 * Every stage below has a ground-truth value from tools/ci/mldsa_sign_ref.py,
 * which reproduces NIST's ACVP deterministic sigGen vectors byte-exactly. That
 * is the DDR-1052 discipline: prove it in Python, where a wrong intermediate is
 * visible, before porting to freestanding C where the only feedback is a
 * 2420-byte answer that either matches or does not.
 * ======================================================================== */
#include "mldsa_sig_kat.h"

#define TAU        39
#define GAMMA1     (1 << 17)
#define GAMMA2     ((Q - 1) / 88)          /* 95232 */
#define OMEGA      80
#define BETA       (TAU * ETA)             /* 78 */
#define CTILDE_LEN 32u                     /* lambda/4, lambda = 128 */

/* An unbounded rejection loop is an S2 violation in this codebase (DDR-961,
 * DDR-994). The expected iteration count for ML-DSA-44 is about 4.25; 1000 is
 * far beyond any plausible run and still a hard ceiling. */
#define SIGN_MAX_ITERS 1000

static unsigned bitlen_u(uint32_t x) {
    unsigned n = 0;
    while (x) { n++; x >>= 1; }
    return n;
}

/* SimpleBitUnpack: `bits`-wide little-endian fields out of a byte stream. */
static void simple_bitunpack(const uint8_t *in, unsigned bits, int32_t *out) {
    uint32_t acc = 0;
    unsigned nb = 0, i = 0, n = 0;
    while (n < MLDSA_N) {
        while (nb < bits) { acc |= (uint32_t)in[i++] << nb; nb += 8; }
        out[n++] = (int32_t)(acc & ((1u << bits) - 1u));
        acc >>= bits; nb -= bits;
    }
}

/* BitUnpack(v, a, b): coefficients are b - z, z a bitlen(a+b)-wide field. */
static void bitunpack(const uint8_t *in, uint32_t a, uint32_t b, int32_t *out) {
    unsigned bits = bitlen_u(a + b);
    simple_bitunpack(in, bits, out);
    for (unsigned i = 0; i < MLDSA_N; i++) out[i] = (int32_t)b - out[i];
}

/* BitPack(w, a, b): the inverse. `w` is CENTERED, in [-a, b]. */
static void bitpack_ab(const int32_t *w, uint32_t a, uint32_t b, uint8_t *out) {
    unsigned bits = bitlen_u(a + b);
    uint32_t acc = 0;
    unsigned nb = 0, o = 0;
    for (unsigned i = 0; i < MLDSA_N; i++) {
        acc |= ((uint32_t)((int32_t)b - w[i]) & ((1u << bits) - 1u)) << nb;
        nb += bits;
        while (nb >= 8u) { out[o++] = (uint8_t)(acc & 0xFFu); acc >>= 8; nb -= 8u; }
    }
    if (nb) out[o] = (uint8_t)(acc & 0xFFu);
}

/* Decompose (FIPS 204 Alg. 36). r1 = HighBits, r0 = LowBits (centered). */
static void decompose(uint32_t r, int32_t *r1, int32_t *r0) {
    uint32_t rp = r % (uint32_t)Q;
    int32_t  lo = (int32_t)(rp % (2u * (uint32_t)GAMMA2));
    if (lo > GAMMA2) lo -= 2 * GAMMA2;
    if ((int32_t)rp - lo == Q - 1) { *r1 = 0; *r0 = lo - 1; return; }
    *r1 = ((int32_t)rp - lo) / (2 * GAMMA2);
    *r0 = lo;
}

/* Infinity norm of a poly held as residues mod q: min(c, q-c) per coefficient. */
static uint32_t inf_norm(const int32_t *p) {
    uint32_t worst = 0;
    for (unsigned i = 0; i < MLDSA_N; i++) {
        uint32_t c = (uint32_t)p[i] % (uint32_t)Q;
        uint32_t v = (c > (uint32_t)Q - c) ? (uint32_t)Q - c : c;
        if (v > worst) worst = v;
    }
    return worst;
}

/* SampleInBall (FIPS 204 Alg. 29). STREAMED: the rejection `while j > i` loop
 * has no fixed byte budget, so a fixed squeeze would be a bound by luck. */
static void sample_in_ball(const uint8_t ct[CTILDE_LEN], int32_t *c) {
    keccak_ctx x;
    uint8_t sgn[8];
    uint64_t bits = 0;
    unsigned i;

    for (i = 0; i < MLDSA_N; i++) c[i] = 0;
    shake256_init(&x);
    keccak_update(&x, ct, CTILDE_LEN);
    keccak_squeeze(&x, sgn, 8);
    for (i = 0; i < 8u; i++) bits |= (uint64_t)sgn[i] << (8u * i);

    for (i = MLDSA_N - TAU; i < MLDSA_N; i++) {
        uint8_t j;
        do { keccak_squeeze(&x, &j, 1); } while (j > i);
        c[i] = c[j];
        c[j] = (bits & 1u) ? (Q - 1) : 1;      /* 1 - 2*bit, as a residue */
        bits >>= 1;
    }
}

/* MakeHint (FIPS 204 Alg. 39): does adding z move r into a different HighBits? */
static uint8_t make_hint(uint32_t z, uint32_t r) {
    int32_t r1, r0, v1, v0;
    decompose(r, &r1, &r0);
    decompose((uint32_t)(((uint64_t)r + z) % (uint64_t)Q), &v1, &v0);
    return (uint8_t)(v1 != r1);
}

/* Pointwise multiply-accumulate in the NTT domain. */
static void pointwise(int32_t *dst, const int32_t *a, const int32_t *b) {
    for (unsigned j = 0; j < MLDSA_N; j++)
        dst[j] = (int32_t)modmul((uint32_t)a[j], (uint32_t)b[j]);
}

int mldsa44_sign_internal(const uint8_t sk[MLDSA44_SK_BYTES],
                          const uint8_t *msg, unsigned long msglen,
                          uint8_t sig[MLDSA44_SIG_BYTES],
                          mldsa44_sign_scratch *sc)
{
    const uint8_t *rho = sk, *kk = sk + 32, *tr = sk + 64;
    uint8_t mu[64], rhopp[64], ctilde[CTILDE_LEN];
    uint8_t rnd[32];
    keccak_ctx x;
    unsigned r, s, j, o, iter;

    if (!sc->tables_ready) { tables_fill(sc->zetas, &sc->ninv); sc->tables_ready = 1; }

    /* skDecode. s1/s2 are eta-centered; t0 is 2^(d-1)-centered. */
    o = 128;
    for (s = 0; s < MLDSA_L; s++) {
        bitunpack(sk + o, ETA, ETA, sc->s1h[s]); o += 96;
        for (j = 0; j < MLDSA_N; j++)
            if (sc->s1h[s][j] < 0) sc->s1h[s][j] += Q;
        ntt(sc->zetas, sc->s1h[s]);
    }
    for (r = 0; r < MLDSA_K; r++) {
        bitunpack(sk + o, ETA, ETA, sc->s2h[r]); o += 96;
        for (j = 0; j < MLDSA_N; j++)
            if (sc->s2h[r][j] < 0) sc->s2h[r][j] += Q;
        ntt(sc->zetas, sc->s2h[r]);
    }
    for (r = 0; r < MLDSA_K; r++) {
        bitunpack(sk + o, (1u << (D - 1)) - 1u, 1u << (D - 1), sc->t0h[r]); o += 416;
        for (j = 0; j < MLDSA_N; j++)
            if (sc->t0h[r][j] < 0) sc->t0h[r][j] += Q;
        ntt(sc->zetas, sc->t0h[r]);
    }

    /* mu = H(tr || M, 64); rho'' = H(K || rnd || mu, 64), rnd = 0^32. */
    shake256_init(&x);
    keccak_update(&x, tr, 64);
    keccak_update(&x, msg, (size_t)msglen);
    keccak_squeeze(&x, mu, 64);

    for (j = 0; j < 32u; j++) rnd[j] = 0;
    shake256_init(&x);
    keccak_update(&x, kk, 32);
    keccak_update(&x, rnd, 32);
    keccak_update(&x, mu, 64);
    keccak_squeeze(&x, rhopp, 64);

    for (iter = 0; iter < SIGN_MAX_ITERS; iter++) {
        unsigned kappa = iter * MLDSA_L;
        unsigned w1bits = bitlen_u((uint32_t)((Q - 1) / (2 * GAMMA2) - 1));  /* 6 */
        unsigned hint_total = 0;
        int reject = 0;

        /* y = ExpandMask(rho'', kappa) */
        for (s = 0; s < MLDSA_L; s++) {
            uint8_t v[32 * 18];
            uint8_t idx[2];
            unsigned m = kappa + s;
            shake256_init(&x);
            keccak_update(&x, rhopp, 64);
            idx[0] = (uint8_t)(m & 0xFFu); idx[1] = (uint8_t)(m >> 8);
            keccak_update(&x, idx, 2);
            keccak_squeeze(&x, v, sizeof v);
            bitunpack(v, GAMMA1 - 1, GAMMA1, sc->y[s]);
            for (j = 0; j < MLDSA_N; j++) {
                sc->yh[s][j] = sc->y[s][j] < 0 ? sc->y[s][j] + Q : sc->y[s][j];
                sc->y[s][j]  = sc->yh[s][j];
            }
            ntt(sc->zetas, sc->yh[s]);
        }

        /* w = NTT^-1(A . NTT(y)); w1 = HighBits(w) */
        for (r = 0; r < MLDSA_K; r++) {
            for (j = 0; j < MLDSA_N; j++) sc->acc[j] = 0;
            for (s = 0; s < MLDSA_L; s++) {
                rej_ntt_poly(rho, (uint8_t)s, (uint8_t)r, sc->aij);
                for (j = 0; j < MLDSA_N; j++)
                    sc->acc[j] = (int32_t)(((uint32_t)sc->acc[j]
                                  + modmul((uint32_t)sc->aij[j],
                                           (uint32_t)sc->yh[s][j])) % Q);
            }
            intt(sc->zetas, sc->ninv, sc->acc);
            for (j = 0; j < MLDSA_N; j++) {
                int32_t r1, r0;
                sc->w[r][j] = sc->acc[j];
                decompose((uint32_t)sc->acc[j], &r1, &r0);
                sc->w1[r][j] = r1;
            }
            bitpack_simple(sc->w1[r], w1bits, sc->w1enc + r * 192);
        }

        /* c~ = H(mu || w1Encode(w1)); c = SampleInBall(c~) */
        shake256_init(&x);
        keccak_update(&x, mu, 64);
        keccak_update(&x, sc->w1enc, MLDSA_K * 192);
        keccak_squeeze(&x, ctilde, CTILDE_LEN);
        sample_in_ball(ctilde, sc->c);
        for (j = 0; j < MLDSA_N; j++) sc->ch[j] = sc->c[j];
        ntt(sc->zetas, sc->ch);

        /* z = y + c*s1 */
        for (s = 0; s < MLDSA_L; s++) {
            pointwise(sc->cs1[s], sc->ch, sc->s1h[s]);
            intt(sc->zetas, sc->ninv, sc->cs1[s]);
            for (j = 0; j < MLDSA_N; j++)
                sc->z[s][j] = (int32_t)(((uint32_t)sc->y[s][j]
                                        + (uint32_t)sc->cs1[s][j]) % Q);
        }
        for (s = 0; s < MLDSA_L; s++)
            if (inf_norm(sc->z[s]) >= (uint32_t)(GAMMA1 - BETA)) reject = 1;

        /* r0 = LowBits(w - c*s2) */
        for (r = 0; r < MLDSA_K && !reject; r++) {
            pointwise(sc->cs2[r], sc->ch, sc->s2h[r]);
            intt(sc->zetas, sc->ninv, sc->cs2[r]);
            for (j = 0; j < MLDSA_N; j++) {
                int32_t r1, r0;
                uint32_t v = ((uint32_t)sc->w[r][j] + (uint32_t)Q
                              - (uint32_t)sc->cs2[r][j]) % (uint32_t)Q;
                decompose(v, &r1, &r0);
                if ((uint32_t)(r0 < 0 ? -r0 : r0) >= (uint32_t)(GAMMA2 - BETA))
                    reject = 1;
            }
        }
        if (reject) continue;

        /* Recompute cs2 for every row -- the loop above may have exited early. */
        for (r = 0; r < MLDSA_K; r++) {
            pointwise(sc->cs2[r], sc->ch, sc->s2h[r]);
            intt(sc->zetas, sc->ninv, sc->cs2[r]);
            pointwise(sc->ct0[r], sc->ch, sc->t0h[r]);
            intt(sc->zetas, sc->ninv, sc->ct0[r]);
        }
        for (r = 0; r < MLDSA_K; r++)
            if (inf_norm(sc->ct0[r]) >= (uint32_t)GAMMA2) reject = 1;
        if (reject) continue;

        for (r = 0; r < MLDSA_K; r++) {
            for (j = 0; j < MLDSA_N; j++) {
                uint32_t negct0 = ((uint32_t)Q - (uint32_t)sc->ct0[r][j]) % (uint32_t)Q;
                uint32_t rv = (((uint32_t)sc->w[r][j] + (uint32_t)Q
                                - (uint32_t)sc->cs2[r][j]) % (uint32_t)Q
                               + (uint32_t)sc->ct0[r][j]) % (uint32_t)Q;
                sc->hint[r][j] = make_hint(negct0, rv);
                hint_total += sc->hint[r][j];
            }
        }
        if (hint_total > OMEGA) continue;

        /* sigEncode(c~, z mod+- q, h) */
        for (j = 0; j < CTILDE_LEN; j++) sig[j] = ctilde[j];
        o = CTILDE_LEN;
        for (s = 0; s < MLDSA_L; s++) {
            for (j = 0; j < MLDSA_N; j++)
                if (sc->z[s][j] > Q / 2) sc->z[s][j] -= Q;
            bitpack_ab(sc->z[s], GAMMA1 - 1, GAMMA1, sig + o);
            o += 576;
        }
        for (j = 0; j < (unsigned)(OMEGA + MLDSA_K); j++) sig[o + j] = 0;
        {   unsigned at = 0;
            for (r = 0; r < MLDSA_K; r++) {
                for (j = 0; j < MLDSA_N; j++)
                    if (sc->hint[r][j]) {
                        /* The `hint_total > OMEGA` test above is what keeps this
                         * in bounds -- and the pinned vectors NEVER trigger it
                         * (measured: a mutant defeating that test still passes
                         * the KAT arm), so it is an untested guard on a write
                         * into a fixed 2420-byte buffer. Re-check here rather
                         * than trust it: past OMEGA the index run would overrun
                         * into the per-row counts and then past the signature. */
                        if (at >= (unsigned)OMEGA) return -1;
                        sig[o + at++] = (uint8_t)j;
                    }
                sig[o + OMEGA + r] = (uint8_t)at;
            }
        }
        return 0;
    }
    return -1;                       /* bound reached; see SIGN_MAX_ITERS */
}

static const struct {
    const uint8_t *sk, *msg, *sig;
    unsigned msglen;
} MLDSA44_SIG_KATS[MLDSA44_SIG_KAT_COUNT] = {
    { MLDSA44_SIGSK_110, MLDSA44_SIGMSG_110, MLDSA44_SIG_110, MLDSA44_SIGMSGLEN_110 },
    { MLDSA44_SIGSK_118, MLDSA44_SIGMSG_118, MLDSA44_SIG_118, MLDSA44_SIGMSGLEN_118 },
};
_Static_assert(MLDSA44_SIG_KAT_COUNT == 2,
               "mldsa_sig_kat.h gained or lost vectors; extend the table above");

/* Decompose boundary arm. THE SIGNING KATs DO NOT COVER THIS, measured the same
 * way DDR-1054 measured Power2Round's: `lo == GAMMA2` exactly occurs in 0 of the
 * 28,672 decompose calls across both pinned vectors (~0.15 expected), so a
 * mutant flipping this comparison from `>` to `>=` passes the KAT arm outright.
 * These twelve cases are generated from FIPS 204 Alg. 36 and bracket the
 * boundary from both sides; r = q-1 additionally covers the special branch where
 * r+ - r0 == q-1. */
static const struct { uint32_t r; int32_t r1, r0; } DECOMP_VEC[] = {
    {        0u, 0,      0 },
    {        1u, 0,      1 },
    {    95231u, 0,  95231 },
    {    95232u, 0,  95232 },   /* the boundary itself: lo == GAMMA2        */
    {    95233u, 1, -95231 },   /* one past it, where the branch DOES fire  */
    {   190464u, 1,      0 },
    {   190465u, 1,      1 },
    {   285696u, 1,  95232 },   /* the boundary again, one limb up          */
    {   380928u, 2,      0 },
    {  8380416u, 0,     -1 },   /* q-1: the r+ - r0 == q-1 special branch   */
    {  8285185u, 0, -95232 },
    {  1234567u, 6,  91783 },
};

static int decompose_selftest(void) {
    for (unsigned i = 0; i < sizeof DECOMP_VEC / sizeof DECOMP_VEC[0]; i++) {
        int32_t r1 = 0, r0 = 0;
        decompose(DECOMP_VEC[i].r, &r1, &r0);
        if (r1 != DECOMP_VEC[i].r1 || r0 != DECOMP_VEC[i].r0) return (int)i + 1;
    }
    return 0;
}

unsigned mldsa44_sig_kat_count(void) { return MLDSA44_SIG_KAT_COUNT; }
unsigned mldsa44_decomp_count(void) {
    return (unsigned)(sizeof DECOMP_VEC / sizeof DECOMP_VEC[0]);
}

int mldsa44_sign_selftest(mldsa44_sign_scratch *sc) {
    /* Boundary arm first: deterministic, instant, and a failure here localises
     * far better than a 2420-byte mismatch. NEGATIVE index, so the two arms
     * cannot be confused in a log. */
    { int d = decompose_selftest(); if (d) return -d; }

    for (unsigned v = 0; v < MLDSA44_SIG_KAT_COUNT; v++) {
        if (mldsa44_sign_internal(MLDSA44_SIG_KATS[v].sk, MLDSA44_SIG_KATS[v].msg,
                                  MLDSA44_SIG_KATS[v].msglen, sc->sig, sc) != 0)
            return (int)v + 1;
        for (unsigned i = 0; i < MLDSA44_SIG_BYTES; i++)
            if (sc->sig[i] != MLDSA44_SIG_KATS[v].sig[i]) return (int)v + 1;
    }
    return 0;
}

/* ========================================================================
 * DDR-1058 — ML-DSA.Verify_internal (FIPS 204 Alg. 8).
 *
 * The ACVP sigVer set is 3 signatures that must verify and 12 that must not,
 * so the NEGATIVE cases are the load-bearing half: an implementation that
 * always answers "valid" passes every positive test. The pinned set keeps both
 * verdicts for exactly that reason.
 * ======================================================================== */
#include "mldsa_ver_kat.h"

/* HintBitUnpack (FIPS 204 Alg. 21). Returns 0 on a MALFORMED encoding, which is
 * a rejection and not a fault: indices must strictly increase within a row, the
 * per-row cumulative counts must be non-decreasing and at most OMEGA, and the
 * unused tail must be zero. Those checks are the reason a verifier cannot be
 * fed an arbitrary byte string. */
static int hint_bit_unpack(const uint8_t *y, uint8_t h[MLDSA_K][MLDSA_N]) {
    unsigned index = 0, i, j;

    for (i = 0; i < MLDSA_K; i++)
        for (j = 0; j < MLDSA_N; j++) h[i][j] = 0;

    for (i = 0; i < MLDSA_K; i++) {
        unsigned end = y[OMEGA + i];
        if (end < index || end > (unsigned)OMEGA) return 0;
        {   unsigned first = index;
            while (index < end) {
                if (index > first && y[index - 1] >= y[index]) return 0;
                h[i][y[index]] = 1;
                index++;
            }
        }
    }
    for (i = index; i < (unsigned)OMEGA; i++)
        if (y[i] != 0) return 0;
    return 1;
}

/* UseHint (FIPS 204 Alg. 40). */
static int32_t use_hint(uint8_t hbit, uint32_t r) {
    int32_t mm = (Q - 1) / (2 * GAMMA2);
    int32_t r1, r0;
    decompose(r, &r1, &r0);
    if (!hbit) return r1;
    if (r0 > 0) return (r1 + 1) % mm;
    return (r1 - 1 + mm) % mm;
}

int mldsa44_verify_internal(const uint8_t pk[MLDSA44_PK_BYTES],
                            const uint8_t *msg, unsigned long msglen,
                            const uint8_t sig[MLDSA44_SIG_BYTES],
                            mldsa44_verify_scratch *sc)
{
    const uint8_t *rho = pk;
    uint8_t tr[64], mu[64], ct2[CTILDE_LEN];
    keccak_ctx x;
    unsigned r, s, j, o;
    unsigned w1bits = bitlen_u((uint32_t)((Q - 1) / (2 * GAMMA2) - 1));

    if (!sc->tables_ready) { tables_fill(sc->zetas, &sc->ninv); sc->tables_ready = 1; }

    /* pkDecode */
    o = 32;
    for (r = 0; r < MLDSA_K; r++) { simple_bitunpack(pk + o, 10, sc->t1[r]); o += 320; }

    /* sigDecode. z first, then the hint -- a malformed hint is a rejection. */
    o = CTILDE_LEN;
    for (s = 0; s < MLDSA_L; s++) {
        bitunpack(sig + o, GAMMA1 - 1, GAMMA1, sc->z[s]); o += 576;
    }
    if (!hint_bit_unpack(sig + o, sc->hint)) return 0;

    /* ||z||inf < gamma1 - beta, on the CENTERED values sigDecode produced. */
    for (s = 0; s < MLDSA_L; s++)
        for (j = 0; j < MLDSA_N; j++) {
            int32_t v = sc->z[s][j] < 0 ? -sc->z[s][j] : sc->z[s][j];
            if (v >= GAMMA1 - BETA) return 0;
        }

    /* mu = H(H(pk, 64) || M, 64) */
    shake256(pk, MLDSA44_PK_BYTES, tr, 64);
    shake256_init(&x);
    keccak_update(&x, tr, 64);
    keccak_update(&x, msg, (size_t)msglen);
    keccak_squeeze(&x, mu, 64);

    sample_in_ball(sig, sc->c);           /* c~ is the first CTILDE_LEN bytes */
    for (j = 0; j < MLDSA_N; j++) sc->ch[j] = sc->c[j];
    ntt(sc->zetas, sc->ch);

    for (s = 0; s < MLDSA_L; s++) {
        for (j = 0; j < MLDSA_N; j++)
            sc->zh[s][j] = sc->z[s][j] < 0 ? sc->z[s][j] + Q : sc->z[s][j];
        ntt(sc->zetas, sc->zh[s]);
    }
    for (r = 0; r < MLDSA_K; r++) {
        for (j = 0; j < MLDSA_N; j++)
            sc->t1h[r][j] = (int32_t)(((uint64_t)sc->t1[r][j] << D) % (uint64_t)Q);
        ntt(sc->zetas, sc->t1h[r]);
    }

    /* w'approx = NTT^-1(A.z-hat - c-hat.(t1 * 2^d)); w1' = UseHint(h, w'approx) */
    for (r = 0; r < MLDSA_K; r++) {
        for (j = 0; j < MLDSA_N; j++) sc->acc[j] = 0;
        for (s = 0; s < MLDSA_L; s++) {
            rej_ntt_poly(rho, (uint8_t)s, (uint8_t)r, sc->aij);
            for (j = 0; j < MLDSA_N; j++)
                sc->acc[j] = (int32_t)(((uint32_t)sc->acc[j]
                              + modmul((uint32_t)sc->aij[j],
                                       (uint32_t)sc->zh[s][j])) % Q);
        }
        for (j = 0; j < MLDSA_N; j++) {
            uint32_t t = modmul((uint32_t)sc->ch[j], (uint32_t)sc->t1h[r][j]);
            sc->acc[j] = (int32_t)(((uint32_t)sc->acc[j] + (uint32_t)Q - t) % Q);
        }
        intt(sc->zetas, sc->ninv, sc->acc);
        for (j = 0; j < MLDSA_N; j++)
            sc->w1[r][j] = use_hint(sc->hint[r][j], (uint32_t)sc->acc[j]);
        bitpack_simple(sc->w1[r], w1bits, sc->w1enc + r * 192);
    }

    shake256_init(&x);
    keccak_update(&x, mu, 64);
    keccak_update(&x, sc->w1enc, MLDSA_K * 192);
    keccak_squeeze(&x, ct2, CTILDE_LEN);

    for (j = 0; j < CTILDE_LEN; j++)
        if (ct2[j] != sig[j]) return 0;
    return 1;
}

static const struct {
    const uint8_t *pk, *msg, *sig;
    unsigned msglen;
    int expect;
} MLDSA44_VER_KATS[MLDSA44_VER_KAT_COUNT] = {
    { MLDSA44_VPK_116, MLDSA44_VMSG_116, MLDSA44_VSIG_116, MLDSA44_VMSGLEN_116, MLDSA44_VOK_116 },
    { MLDSA44_VPK_114, MLDSA44_VMSG_114, MLDSA44_VSIG_114, MLDSA44_VMSGLEN_114, MLDSA44_VOK_114 },
    { MLDSA44_VPK_120, MLDSA44_VMSG_120, MLDSA44_VSIG_120, MLDSA44_VMSGLEN_120, MLDSA44_VOK_120 },
    { MLDSA44_VPK_107, MLDSA44_VMSG_107, MLDSA44_VSIG_107, MLDSA44_VMSGLEN_107, MLDSA44_VOK_107 },
    { MLDSA44_VPK_117, MLDSA44_VMSG_117, MLDSA44_VSIG_117, MLDSA44_VMSGLEN_117, MLDSA44_VOK_117 },
};
_Static_assert(MLDSA44_VER_KAT_COUNT == 5,
               "mldsa_ver_kat.h gained or lost vectors; extend the table above");

/* UseHint boundary arm. The ACVP verify vectors do NOT reach r0 == 0 with the
 * hint bit set -- measured: a mutant flipping `r0 > 0` to `r0 >= 0` passes all
 * five. Same shape as DDR-1054's Power2Round and DDR-1057's Decompose, now in a
 * third function, which is why these arms are becoming a habit rather than a
 * one-off. r = 190464 is the boundary: r0 == 0 exactly, so h=1 must take the
 * r0 <= 0 branch and yield 0, where the mutant yields 2. */
static const struct { uint8_t h; uint32_t r; int32_t want; } USEHINT_VEC[] = {
    { 0,        0u,  0 },
    { 0,        1u,  0 },
    { 1,        0u, 43 },
    { 1,        1u,  1 },
    { 1,   190464u,  0 },   /* r0 == 0 exactly, hint set: THE boundary */
    { 1,   190465u,  2 },
    { 1,   190463u,  0 },
    { 0,   190464u,  1 },
    { 1,   285696u,  2 },
    { 1,  8380416u, 43 },   /* q-1, where Decompose takes its special branch */
    { 1,   380928u,  1 },
    { 0,  1234567u,  6 },
    { 1,  1234567u,  7 },
};

static int usehint_selftest(void) {
    for (unsigned i = 0; i < sizeof USEHINT_VEC / sizeof USEHINT_VEC[0]; i++)
        if (use_hint(USEHINT_VEC[i].h, USEHINT_VEC[i].r) != USEHINT_VEC[i].want)
            return (int)i + 1;
    return 0;
}

unsigned mldsa44_ver_kat_count(void) { return MLDSA44_VER_KAT_COUNT; }
unsigned mldsa44_usehint_count(void) {
    return (unsigned)(sizeof USEHINT_VEC / sizeof USEHINT_VEC[0]);
}

int mldsa44_verify_selftest(mldsa44_verify_scratch *sc) {
    /* Boundary arm first, NEGATIVE index -- see DDR-1058 §4. */
    { int u = usehint_selftest(); if (u) return -u; }

    for (unsigned v = 0; v < MLDSA44_VER_KAT_COUNT; v++) {
        int got = mldsa44_verify_internal(MLDSA44_VER_KATS[v].pk,
                                          MLDSA44_VER_KATS[v].msg,
                                          MLDSA44_VER_KATS[v].msglen,
                                          MLDSA44_VER_KATS[v].sig, sc);
        /* BOTH directions. A verifier that always accepts fails the REJECT
         * vectors; one that always rejects fails the ACCEPT vectors. */
        if (got != MLDSA44_VER_KATS[v].expect) return (int)v + 1;
    }
    return 0;
}
