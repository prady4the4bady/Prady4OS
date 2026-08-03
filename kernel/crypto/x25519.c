/* kernel/crypto/x25519.c — X25519 (RFC 7748), DDR-820.
 *
 * Field arithmetic mod p = 2^255 - 19 in 5 limbs of 51 bits over uint64_t, with
 * unsigned __int128 products. Chosen over the 10-limb 25.5-bit ref10 layout
 * because every ADR-034 target ISA is 64-bit: the 5x51 code has materially
 * fewer carry chains, and carry propagation is where a hand-written field
 * implementation is most likely to be subtly wrong in a way short vectors miss.
 *
 * Nothing here branches on, or indexes memory by, a secret bit. See x25519.h
 * for what that does and does NOT buy, and why no gate can check it.
 */
#include "x25519.h"
#include "fe25519.h"   /* DDR-821: field layer extracted so ed25519.c shares it */


int x25519_scalarmult(uint8_t out[X25519_LEN],
                      const uint8_t scalar[X25519_LEN],
                      const uint8_t point[X25519_LEN]) {
    uint8_t e[32];
    for (unsigned i = 0; i < 32u; i++)
        e[i] = scalar[i];
    /* RFC 7748 §5 clamping. Clearing the low 3 bits puts the scalar in the
     * right cofactor class; setting bit 254 and clearing bit 255 fixes the
     * ladder's length so it does not depend on the secret. */
    e[0]  &= 248u;
    e[31] &= 127u;
    e[31] |= 64u;

    fe x1, x2, z2, x3, z3, tmp0, tmp1;
    fe_frombytes(x1, point);
    fe_1(x2); fe_0(z2);
    fe_copy(x3, x1); fe_1(z3);

    uint64_t swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        uint64_t b = (uint64_t)((e[pos / 8] >> (pos & 7)) & 1u);
        swap ^= b;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = b;

        /* RFC 7748 §5 ladder step, verbatim in structure. */
        fe_sub(tmp0, x3, z3);
        fe_sub(tmp1, x2, z2);
        fe_add(x2,   x2, z2);
        fe_add(z2,   x3, z3);
        fe_mul(z3, tmp0, x2);
        fe_mul(z2, z2,   tmp1);
        fe_sq(tmp0, tmp1);
        fe_sq(tmp1, x2);
        fe_add(x3, z3, z2);
        fe_sub(z2, z3, z2);
        fe_mul(x2, tmp1, tmp0);
        fe_sub(tmp1, tmp1, tmp0);
        fe_sq(z2, z2);
        fe_mul121666(z3, tmp1);
        fe_sq(x3, x3);
        fe_add(tmp0, tmp0, z3);
        fe_mul(z3, x1, z2);
        fe_mul(z2, tmp1, tmp0);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);

    /* Contributory check. An all-zero result means the peer sent a small-order
     * point and CHOSE the shared secret rather than agreeing to it — every key
     * derived from it would be one the peer already knows. Accumulate with OR
     * so the check itself does not branch per byte. */
    uint8_t acc = 0;
    for (unsigned i = 0; i < 32u; i++)
        acc |= out[i];
    return acc ? 0 : -1;
}

int x25519_scalarmult_base(uint8_t out[X25519_LEN],
                           const uint8_t scalar[X25519_LEN]) {
    uint8_t base[32] = { 9 };          /* u = 9; remaining bytes zero */
    return x25519_scalarmult(out, scalar, base);
}
