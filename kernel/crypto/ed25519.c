/* kernel/crypto/ed25519.c — Ed25519 (RFC 8032), DDR-821.
 *
 * Twisted Edwards curve -x^2 + y^2 = 1 + d x^2 y^2 over p = 2^255 - 19, using
 * the field layer extracted to fe25519.{c,h} (DDR-821 step 1) and SHA-512.
 *
 * CURVE CONSTANTS ARE DERIVED, NOT RECALLED. Every prior primitive in this tree
 * carries the caveat "these constants are recalled, not fetched", and DDR-820
 * and DDR-819 both shipped a wrong recalled constant that a vector caught. Here
 * that risk is removed at the source rather than tested for:
 *
 *   d  = -121665 / 121666      computed with fe_invert at init
 *   By = 4/5                   computed with fe_invert at init
 *   Bx = the even square root of (y^2 - 1)/(d y^2 + 1)
 *
 * so nothing in this file depends on a 32-byte constant being remembered
 * correctly. The ONE unavoidable recalled constant is the group order L, which
 * is not derivable from the curve equation; it is isolated in `L_BYTES` and is
 * exercised by every signature the vectors check.
 *
 * Nothing here branches on, or indexes memory by, a secret. See ed25519.h for
 * what that buys and what it does not.
 */
#include "ed25519.h"
#include "fe25519.h"
#include "sha512.h"

/* ---------------- small field helpers ---------------- */

static void fe_neg(fe h, const fe f) { fe z; fe_0(z); fe_sub(h, z, f); }

static void fe_small(fe h, uint32_t v) { fe_0(h); h[0] = v; }

/* h = f^(2^n) */
static void fe_sqn(fe h, const fe f, unsigned n) {
    fe_sq(h, f);
    for (unsigned i = 1; i < n; i++)
        fe_sq(h, h);
}

/* z^((p-5)/8), the exponent the p = 5 (mod 8) square-root recipe needs.
 * (p-5)/8 = 2^252 - 3. Built from the same addition-chain shape fe_invert uses;
 * the exponent is public so the fixed sequence is constant-time in the only
 * sense that matters. */
static void fe_pow2523(fe out, const fe z) {
    fe t0, t1, t2;
    fe_sq(t0, z);
    fe_sq(t1, t0); fe_sq(t1, t1);
    fe_mul(t1, z, t1);
    fe_mul(t0, t0, t1);
    fe_sq(t0, t0);
    fe_mul(t0, t1, t0);
    fe_sqn(t1, t0, 5);   fe_mul(t0, t1, t0);
    fe_sqn(t1, t0, 10);  fe_mul(t1, t1, t0);
    fe_sqn(t2, t1, 20);  fe_mul(t1, t2, t1);
    fe_sqn(t1, t1, 10);  fe_mul(t0, t1, t0);
    fe_sqn(t1, t0, 50);  fe_mul(t1, t1, t0);
    fe_sqn(t2, t1, 100); fe_mul(t1, t2, t1);
    fe_sqn(t1, t1, 50);  fe_mul(t0, t1, t0);
    fe_sq(t0, t0); fe_sq(t0, t0);
    fe_mul(out, t0, z);
}

static int fe_iszero(const fe f) {
    uint8_t s[32];
    fe_tobytes(s, f);
    uint8_t acc = 0;
    for (unsigned i = 0; i < 32u; i++)
        acc |= s[i];
    return acc == 0;
}

static int fe_eq(const fe a, const fe b) {
    fe t; fe_sub(t, a, b); return fe_iszero(t);
}

static unsigned fe_isodd(const fe f) {
    uint8_t s[32];
    fe_tobytes(s, f);
    return s[0] & 1u;
}

/* ---------------- curve constants, derived once ---------------- */

typedef struct { fe X, Y, Z, T; } ge;      /* extended coords: x=X/Z, y=Y/Z */

static fe C_D, C_SQRTM1;
static ge  C_B;
static int g_init;

static void ge_zero(ge *p) { fe_0(p->X); fe_1(p->Y); fe_1(p->Z); fe_0(p->T); }

/* sqrt(-1) = 2^((p-1)/4). Derived, not recalled. */
static void derive_sqrtm1(fe out) {
    fe two, t;
    fe_small(two, 2);
    /* 2^((p-1)/4): (p-1)/4 = 2^253 - 6. Reached as (2^((p-5)/8))^2 * 2. */
    fe_pow2523(t, two);
    fe_sq(t, t);
    fe_mul(out, t, two);
}

static void ge_add(ge *r, const ge *p, const ge *q);

static void curve_init(void) {
    if (g_init) return;

    /* d = -121665 * inv(121666) */
    fe a, b;
    fe_small(a, 121665); fe_neg(a, a);
    fe_small(b, 121666); fe_invert(b, b);
    fe_mul(C_D, a, b);

    derive_sqrtm1(C_SQRTM1);

    /* By = 4/5; Bx = even sqrt((y^2-1)/(d y^2 + 1)) */
    fe four, five, y, y2, num, den, x, chk;
    fe_small(four, 4); fe_small(five, 5);
    fe_invert(five, five);
    fe_mul(y, four, five);

    fe_sq(y2, y);
    fe_1(a);
    fe_sub(num, y2, a);                 /* y^2 - 1 */
    fe_mul(den, C_D, y2); fe_add(den, den, a);   /* d y^2 + 1 */
    fe_invert(den, den);
    fe_mul(num, num, den);              /* u = (y^2-1)/(d y^2+1) */

    /* x = u^((p+3)/8) = u * u^((p-5)/8); fix up by sqrt(-1) if needed. */
    fe_pow2523(x, num);
    fe_mul(x, x, num);
    fe_sq(chk, x);
    if (!fe_eq(chk, num))
        fe_mul(x, x, C_SQRTM1);
    if (fe_isodd(x))
        fe_neg(x, x);                   /* RFC 8032: base point has x even */

    fe_copy(C_B.X, x);
    fe_copy(C_B.Y, y);
    fe_1(C_B.Z);
    fe_mul(C_B.T, x, y);

    g_init = 1;
}

/* ---------------- group law (extended coords, a = -1) ---------------- */

/* add-2008-hwcd-3 */
static void ge_add(ge *r, const ge *p, const ge *q) {
    fe A, B, C, D, E, F, G, H, t;
    fe_sub(A, p->Y, p->X); fe_sub(t, q->Y, q->X); fe_mul(A, A, t);
    fe_add(B, p->Y, p->X); fe_add(t, q->Y, q->X); fe_mul(B, B, t);
    fe_mul(C, p->T, q->T); fe_add(t, C_D, C_D); fe_mul(C, C, t);
    fe_mul(D, p->Z, q->Z); fe_add(D, D, D);
    fe_sub(E, B, A);
    fe_sub(F, D, C);
    fe_add(G, D, C);
    fe_add(H, B, A);
    fe_mul(r->X, E, F);
    fe_mul(r->Y, G, H);
    fe_mul(r->T, E, H);
    fe_mul(r->Z, F, G);
}

/* dbl-2008-hwcd */
static void ge_dbl(ge *r, const ge *p) {
    fe A, B, C, D, E, F, G, H, t;
    fe_sq(A, p->X);
    fe_sq(B, p->Y);
    fe_sq(C, p->Z); fe_add(C, C, C);
    fe_neg(D, A);
    fe_add(t, p->X, p->Y); fe_sq(t, t);
    fe_sub(E, t, A); fe_sub(E, E, B);
    fe_add(G, D, B);
    fe_sub(F, G, C);
    fe_sub(H, D, B);
    fe_mul(r->X, E, F);
    fe_mul(r->Y, G, H);
    fe_mul(r->T, E, H);
    fe_mul(r->Z, F, G);
}

/* Constant-time select: r = b ? q : p. */
static void ge_cmov(ge *r, const ge *q, uint64_t b) {
    fe_cswap(r->X, ((ge *)q)->X, 0);   /* no-op; keeps the shape explicit */
    uint64_t m = 0u - (b & 1u);
    for (unsigned i = 0; i < 5u; i++) {
        r->X[i] ^= m & (r->X[i] ^ q->X[i]);
        r->Y[i] ^= m & (r->Y[i] ^ q->Y[i]);
        r->Z[i] ^= m & (r->Z[i] ^ q->Z[i]);
        r->T[i] ^= m & (r->T[i] ^ q->T[i]);
    }
}

/* r = [s]p, MSB-first double-and-always-add. The add is unconditional and the
 * result is selected with a mask, so the sequence of field operations does not
 * depend on the scalar. */
static void ge_scalarmult(ge *r, const uint8_t s[32], const ge *p) {
    ge acc, sum;
    ge_zero(&acc);
    for (int i = 255; i >= 0; i--) {
        ge_dbl(&acc, &acc);
        ge_add(&sum, &acc, p);
        ge_cmov(&acc, &sum, (uint64_t)((s[i >> 3] >> (i & 7)) & 1u));
    }
    fe_copy(r->X, acc.X); fe_copy(r->Y, acc.Y);
    fe_copy(r->Z, acc.Z); fe_copy(r->T, acc.T);
}

static void ge_tobytes(uint8_t out[32], const ge *p) {
    fe zi, x, y;
    fe_invert(zi, p->Z);
    fe_mul(x, p->X, zi);
    fe_mul(y, p->Y, zi);
    fe_tobytes(out, y);
    out[31] |= (uint8_t)(fe_isodd(x) << 7);
}

/* 0 on success, -1 if the encoding is not a curve point. */
static int ge_frombytes(ge *p, const uint8_t s[32]) {
    curve_init();
    fe y, y2, num, den, x, chk, one;
    uint8_t t[32];
    for (unsigned i = 0; i < 32u; i++) t[i] = s[i];
    unsigned xsign = t[31] >> 7;
    t[31] &= 0x7fu;
    fe_frombytes(y, t);

    fe_1(one);
    fe_sq(y2, y);
    fe_sub(num, y2, one);
    fe_mul(den, C_D, y2); fe_add(den, den, one);
    fe_invert(den, den);
    fe_mul(num, num, den);

    fe_pow2523(x, num);
    fe_mul(x, x, num);
    fe_sq(chk, x);
    if (!fe_eq(chk, num)) {
        fe_mul(x, x, C_SQRTM1);
        fe_sq(chk, x);
        if (!fe_eq(chk, num))
            return -1;                  /* not a square: not on the curve */
    }
    if (fe_iszero(x) && xsign)
        return -1;                      /* x = 0 with sign bit set is invalid */
    if (fe_isodd(x) != xsign)
        fe_neg(x, x);

    fe_copy(p->X, x);
    fe_copy(p->Y, y);
    fe_1(p->Z);
    fe_mul(p->T, x, y);
    return 0;
}

/* ---------------- scalars mod L ---------------- */

/* L = 2^252 + 27742317777372353535851937790883648493, little-endian.
 *
 * The one constant in this file that is RECALLED rather than derived — it is
 * the group order and does not follow from the curve equation. Every signature
 * the vectors check exercises it, so a wrong L fails vectors 1-4 immediately. */
static const uint8_t L_BYTES[32] = {
    0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10 };

/* out = a - b over 64-byte little-endian; returns borrow. */
static int bn_sub(uint8_t *out, const uint8_t *a, const uint8_t *b, unsigned n) {
    int borrow = 0;
    for (unsigned i = 0; i < n; i++) {
        int d = (int)a[i] - (int)b[i] - borrow;
        borrow = (d < 0);
        out[i] = (uint8_t)(d & 0xff);
    }
    return borrow;
}

/* r = x mod L, x is 64 bytes little-endian.
 *
 * Binary long division: 512 shift-and-conditional-subtract steps. Deliberately
 * NOT the ref10 21-bit-limb routine — that is several hundred lines of hand
 * transcribed magic constants, and this file's whole discipline is to avoid
 * hand-transcribed constants where a derivation or a simple algorithm will do.
 * It is slower and it is obviously correct. */
static void sc_reduce(uint8_t r[32], const uint8_t x[64]) {
    uint8_t rem[33];
    uint8_t tmp[33];
    uint8_t Lp[33];
    for (unsigned i = 0; i < 33u; i++) rem[i] = 0;
    for (unsigned i = 0; i < 32u; i++) Lp[i] = L_BYTES[i];
    Lp[32] = 0;

    for (int bit = 511; bit >= 0; bit--) {
        /* rem = rem << 1 | bit(x) */
        unsigned carry = (x[bit >> 3] >> (bit & 7)) & 1u;
        for (unsigned i = 0; i < 33u; i++) {
            unsigned v = ((unsigned)rem[i] << 1) | carry;
            rem[i] = (uint8_t)(v & 0xff);
            carry  = (v >> 8) & 1u;
        }
        /* if rem >= L: rem -= L. Branchless on the borrow. */
        int borrow = bn_sub(tmp, rem, Lp, 33);
        uint8_t m = (uint8_t)(borrow - 1);       /* 0xff when rem >= L */
        for (unsigned i = 0; i < 33u; i++)
            rem[i] = (uint8_t)((rem[i] & ~m) | (tmp[i] & m));
    }
    for (unsigned i = 0; i < 32u; i++) r[i] = rem[i];
}

/* s = (a*b + c) mod L */
static void sc_muladd(uint8_t s[32], const uint8_t a[32],
                      const uint8_t b[32], const uint8_t c[32]) {
    uint32_t prod[64];
    for (unsigned i = 0; i < 64u; i++) prod[i] = 0;
    for (unsigned i = 0; i < 32u; i++)
        for (unsigned j = 0; j < 32u; j++)
            prod[i + j] += (uint32_t)a[i] * (uint32_t)b[j];

    uint8_t wide[64];
    uint32_t carry = 0;
    for (unsigned i = 0; i < 64u; i++) {
        uint32_t v = prod[i] + carry;
        wide[i] = (uint8_t)(v & 0xff);
        carry = v >> 8;
    }
    /* add c */
    unsigned cr = 0;
    for (unsigned i = 0; i < 64u; i++) {
        unsigned v = wide[i] + (i < 32u ? c[i] : 0u) + cr;
        wide[i] = (uint8_t)(v & 0xff);
        cr = v >> 8;
    }
    sc_reduce(s, wide);
}

/* ---------------- public API ---------------- */

static void secret_scalar(uint8_t a[32], uint8_t prefix[32],
                          const uint8_t seed[32]) {
    uint8_t h[64];
    sha512(seed, 32, h);
    for (unsigned i = 0; i < 32u; i++) a[i] = h[i];
    a[0]  &= 248u;                     /* RFC 8032 clamping */
    a[31] &= 127u;
    a[31] |= 64u;
    for (unsigned i = 0; i < 32u; i++) prefix[i] = h[32 + i];
}

void ed25519_pubkey(uint8_t pub[32], const uint8_t seed[32]) {
    curve_init();
    uint8_t a[32], prefix[32];
    ge A;
    secret_scalar(a, prefix, seed);
    ge_scalarmult(&A, a, &C_B);
    ge_tobytes(pub, &A);
}

void ed25519_sign(uint8_t sig[64], const void *msg, uint32_t msglen,
                  const uint8_t seed[32]) {
    curve_init();
    uint8_t a[32], prefix[32], pub[32], rh[64], r[32], hram[64], k[32];
    ge R, A;
    sha512_ctx c;

    secret_scalar(a, prefix, seed);
    ge_scalarmult(&A, a, &C_B);
    ge_tobytes(pub, &A);

    /* r = SHA-512(prefix || M) mod L */
    sha512_init(&c);
    sha512_update(&c, prefix, 32);
    sha512_update(&c, msg, msglen);
    sha512_final(&c, rh);
    sc_reduce(r, rh);

    ge_scalarmult(&R, r, &C_B);
    ge_tobytes(sig, &R);                       /* sig[0..31] = R */

    /* k = SHA-512(R || A || M) mod L ; S = (k*a + r) mod L */
    sha512_init(&c);
    sha512_update(&c, sig, 32);
    sha512_update(&c, pub, 32);
    sha512_update(&c, msg, msglen);
    sha512_final(&c, hram);
    sc_reduce(k, hram);

    sc_muladd(sig + 32, k, a, r);
}

int ed25519_verify(const uint8_t sig[64], const void *msg, uint32_t msglen,
                   const uint8_t pub[32]) {
    curve_init();
    ge A, R, sB, kA, rhs;
    uint8_t hram[64], k[32], lhs_b[32], rhs_b[32];
    sha512_ctx c;

    /* S must be canonical: S < L. A non-reduced S is a malleable second
     * signature for the same message, so it is rejected rather than accepted. */
    {
        uint8_t tmp[33], sp[33], Lp[33];
        for (unsigned i = 0; i < 32u; i++) { sp[i] = sig[32 + i]; Lp[i] = L_BYTES[i]; }
        sp[32] = 0; Lp[32] = 0;
        if (!bn_sub(tmp, sp, Lp, 33))          /* no borrow => S >= L */
            return -1;
    }

    if (ge_frombytes(&A, pub) != 0)
        return -1;
    if (ge_frombytes(&R, sig) != 0)
        return -1;

    sha512_init(&c);
    sha512_update(&c, sig, 32);
    sha512_update(&c, pub, 32);
    sha512_update(&c, msg, msglen);
    sha512_final(&c, hram);
    sc_reduce(k, hram);

    /* Check [S]B == R + [k]A */
    ge_scalarmult(&sB, sig + 32, &C_B);
    ge_scalarmult(&kA, k, &A);
    ge_add(&rhs, &R, &kA);

    ge_tobytes(lhs_b, &sB);
    ge_tobytes(rhs_b, &rhs);

    uint8_t diff = 0;
    for (unsigned i = 0; i < 32u; i++)
        diff |= (uint8_t)(lhs_b[i] ^ rhs_b[i]);
    return diff == 0 ? 0 : -1;
}
