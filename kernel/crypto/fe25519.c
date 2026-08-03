/* kernel/crypto/fe25519.c — field arithmetic mod 2^255-19, DDR-821.
 *
 * Moved out of x25519.c UNCHANGED so Ed25519 can share it. The only edits are
 * dropping `static` (these are now a real interface) and renaming MASK51 to
 * FE_MASK51. smoke-x25519 is the regression test for this move: it passed 4/4
 * locally and is CI-proven before the extraction, so any failure after it is
 * attributable to the extraction and nothing else.
 */
#include "fe25519.h"

static uint64_t load64_le(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8u; i++)
        v |= (uint64_t)p[i] << (i * 8);
    return v;
}

void fe_0(fe h) { for (unsigned i = 0; i < 5u; i++) h[i] = 0; }
void fe_1(fe h) { fe_0(h); h[0] = 1; }
void fe_copy(fe h, const fe f) { for (unsigned i = 0; i < 5u; i++) h[i] = f[i]; }

void fe_add(fe h, const fe f, const fe g) {
    for (unsigned i = 0; i < 5u; i++)
        h[i] = f[i] + g[i];
}

/* h = f - g. Adds 2p first so no limb can go negative: 2*(2^51-19) in limb 0
 * and 2*(2^51-1) in the rest. Doing this with a borrow chain instead would be
 * both slower and a place to be wrong. */
void fe_sub(fe h, const fe f, const fe g) {
    h[0] = f[0] + 0xfffffffffffdaULL - g[0];
    for (unsigned i = 1; i < 5u; i++)
        h[i] = f[i] + 0xffffffffffffeULL - g[i];
}

/* Reduce five 128-bit accumulators into 51-bit limbs. The fold of the overflow
 * past bit 255 back into limb 0 costs a factor of 19, since 2^255 == 19 mod p. */
static void fe_carry(fe h, unsigned __int128 t[5]) {
    unsigned __int128 c;
    c = t[0] >> 51; t[0] &= FE_MASK51; t[1] += c;
    c = t[1] >> 51; t[1] &= FE_MASK51; t[2] += c;
    c = t[2] >> 51; t[2] &= FE_MASK51; t[3] += c;
    c = t[3] >> 51; t[3] &= FE_MASK51; t[4] += c;
    c = t[4] >> 51; t[4] &= FE_MASK51; t[0] += (unsigned __int128)19u * c;
    /* One more short pass: the 19*c fold can push limb 0 over 51 bits. */
    c = t[0] >> 51; t[0] &= FE_MASK51; t[1] += c;

    for (unsigned i = 0; i < 5u; i++)
        h[i] = (uint64_t)t[i];
}

void fe_mul(fe h, const fe f, const fe g) {
    const unsigned __int128 f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    const unsigned __int128 g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
    /* Terms that wrap past 2^255 come back multiplied by 19. */
    const unsigned __int128 g1_19 = 19u * g1, g2_19 = 19u * g2,
                            g3_19 = 19u * g3, g4_19 = 19u * g4;
    unsigned __int128 t[5];
    t[0] = f0*g0 + f1*g4_19 + f2*g3_19 + f3*g2_19 + f4*g1_19;
    t[1] = f0*g1 + f1*g0    + f2*g4_19 + f3*g3_19 + f4*g2_19;
    t[2] = f0*g2 + f1*g1    + f2*g0    + f3*g4_19 + f4*g3_19;
    t[3] = f0*g3 + f1*g2    + f2*g1    + f3*g0    + f4*g4_19;
    t[4] = f0*g4 + f1*g3    + f2*g2    + f3*g1    + f4*g0;
    fe_carry(h, t);
}

/* Squaring via fe_mul rather than a dedicated routine: the ladder is not on a
 * hot path here, and a second set of hand-written product terms is a second
 * place to be wrong for no correctness benefit. */
void fe_sq(fe h, const fe f) { fe_mul(h, f, f); }

/* h = f * 121666 = (A+2)/4, the Montgomery ladder constant.
 *
 * NOT 121665. Both constants appear in the literature and they are not
 * interchangeable: the ladder step below computes z_2 = E * (BB + a24 * E),
 * whose derivation from 4XZ((X-Z)^2 + ((A+2)/4)(4XZ)) requires (A+2)/4 = 121666.
 * RFC 7748 §5 writes the algebraically identical z_2 = E * (AA + a24 * E) with
 * a24 = 121665 = (A-2)/4. Pairing 121665 with the BB form — which is what this
 * file did first — is off by exactly E, and it is invisible to a
 * commutativity check because the result is still a consistent group law. */
void fe_mul121666(fe h, const fe f) {
    unsigned __int128 t[5];
    for (unsigned i = 0; i < 5u; i++)
        t[i] = (unsigned __int128)f[i] * 121666u;
    fe_carry(h, t);
}

/* Constant-time conditional swap — the hinge of the whole file. `swap` is 0 or
 * 1 and is a SECRET (a scalar bit), so this must not branch on it. An
 * `if (swap)` here would be correct and would pass every published vector. */
void fe_cswap(fe a, fe b, uint64_t swap) {
    uint64_t m = 0u - swap;                    /* 0 or all-ones */
    for (unsigned i = 0; i < 5u; i++) {
        uint64_t t = m & (a[i] ^ b[i]);
        a[i] ^= t;
        b[i] ^= t;
    }
}

/* h = f^(p-2) = f^-1, by the standard 2^255-21 addition chain. The exponent is
 * a public constant, so the fixed square/multiply sequence is constant-time in
 * the only sense that matters: it does not depend on f. */
void fe_invert(fe out, const fe z) {
    fe z2, z9, z11, z2_5_0, z2_10_0, z2_20_0, z2_50_0, z2_100_0, t;
    unsigned i;

    fe_sq(z2, z);                                  /* 2   */
    fe_sq(t, z2); fe_sq(t, t);                     /* 8   */
    fe_mul(z9, t, z);                              /* 9   */
    fe_mul(z11, z9, z2);                           /* 11  */
    fe_sq(t, z11);                                 /* 22  */
    fe_mul(z2_5_0, t, z9);                         /* 2^5 - 2^0 = 31 */

    fe_sq(t, z2_5_0); for (i = 1; i < 5u; i++) fe_sq(t, t);
    fe_mul(z2_10_0, t, z2_5_0);

    fe_sq(t, z2_10_0); for (i = 1; i < 10u; i++) fe_sq(t, t);
    fe_mul(z2_20_0, t, z2_10_0);

    fe_sq(t, z2_20_0); for (i = 1; i < 20u; i++) fe_sq(t, t);
    fe_mul(t, t, z2_20_0);                         /* 2^40 - 2^0 */

    fe_sq(t, t); for (i = 1; i < 10u; i++) fe_sq(t, t);
    fe_mul(z2_50_0, t, z2_10_0);

    fe_sq(t, z2_50_0); for (i = 1; i < 50u; i++) fe_sq(t, t);
    fe_mul(z2_100_0, t, z2_50_0);

    fe_sq(t, z2_100_0); for (i = 1; i < 100u; i++) fe_sq(t, t);
    fe_mul(t, t, z2_100_0);                        /* 2^200 - 2^0 */

    fe_sq(t, t); for (i = 1; i < 50u; i++) fe_sq(t, t);
    fe_mul(t, t, z2_50_0);                         /* 2^250 - 2^0 */

    fe_sq(t, t); for (i = 1; i < 5u; i++) fe_sq(t, t);
    fe_mul(out, t, z11);                           /* 2^255 - 21 */
}

/* RFC 7748 §5: the u-coordinate's most significant bit MUST be masked. */
void fe_frombytes(fe h, const uint8_t s[32]) {
    h[0] =  load64_le(s)       & FE_MASK51;
    h[1] = (load64_le(s +  6) >>  3) & FE_MASK51;
    h[2] = (load64_le(s + 12) >>  6) & FE_MASK51;
    h[3] = (load64_le(s + 19) >>  1) & FE_MASK51;
    h[4] = (load64_le(s + 24) >> 12) & FE_MASK51;
}

/* Fully reduce mod p, then serialise little-endian. The reduction is the
 * branchless "add 19, see if it carried past 2^255" trick: h is in [0, 2p), so
 * exactly one conditional subtraction of p is needed and the condition is
 * computed as a mask. */
void fe_tobytes(uint8_t s[32], const fe f) {
    uint64_t h[5];
    uint64_t c, q;
    for (unsigned i = 0; i < 5u; i++) h[i] = f[i];

    /* Normalise limbs first. */
    c = h[0] >> 51; h[0] &= FE_MASK51; h[1] += c;
    c = h[1] >> 51; h[1] &= FE_MASK51; h[2] += c;
    c = h[2] >> 51; h[2] &= FE_MASK51; h[3] += c;
    c = h[3] >> 51; h[3] &= FE_MASK51; h[4] += c;
    c = h[4] >> 51; h[4] &= FE_MASK51; h[0] += 19u * c;
    c = h[0] >> 51; h[0] &= FE_MASK51; h[1] += c;

    /* q = 1 iff h >= p. Computed by adding 19 and inspecting the carry out of
     * bit 255, which is exactly the test "h + 19 >= 2^255". */
    q = (h[0] + 19u) >> 51;
    q = (h[1] + q)   >> 51;
    q = (h[2] + q)   >> 51;
    q = (h[3] + q)   >> 51;
    q = (h[4] + q)   >> 51;

    h[0] += 19u * q;
    c = h[0] >> 51; h[0] &= FE_MASK51; h[1] += c;
    c = h[1] >> 51; h[1] &= FE_MASK51; h[2] += c;
    c = h[2] >> 51; h[2] &= FE_MASK51; h[3] += c;
    c = h[3] >> 51; h[3] &= FE_MASK51; h[4] += c;
    h[4] &= FE_MASK51;                       /* drop the 2^255 bit */

    /* Repack 5x51 into 4x64. */
    uint64_t w0 =  h[0]        | (h[1] << 51);
    uint64_t w1 = (h[1] >> 13) | (h[2] << 38);
    uint64_t w2 = (h[2] >> 26) | (h[3] << 25);
    uint64_t w3 = (h[3] >> 39) | (h[4] << 12);
    const uint64_t w[4] = { w0, w1, w2, w3 };
    for (unsigned i = 0; i < 4u; i++)
        for (unsigned j = 0; j < 8u; j++)
            s[i * 8 + j] = (uint8_t)(w[i] >> (j * 8));
}
