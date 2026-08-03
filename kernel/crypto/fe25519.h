/* kernel/crypto/fe25519.h — field arithmetic mod p = 2^255 - 19, DDR-821.
 *
 * Extracted verbatim from x25519.c so Ed25519 can share it. X25519 and Ed25519
 * are different curves with different group laws — Montgomery x-only ladder vs
 * twisted Edwards extended coordinates — but they are the SAME field, and this
 * is that field.
 *
 * Sharing it has a consequence worth stating rather than discovering: a bug
 * here now breaks key agreement AND signing together. That is accepted because
 * this is the best-tested code in the crypto directory — smoke-x25519 exercises
 * it through 7 RFC 7748 checks including a commutativity identity that depends
 * on no published constant — and a second copy would be a second, less-tested
 * place to be wrong.
 *
 * 5 limbs of 51 bits over uint64_t with unsigned __int128 products. Requires a
 * 64-bit target; all three ADR-034 ISAs qualify. Nothing here branches on, or
 * indexes memory by, a secret.
 */
#pragma once
#include <stdint.h>

typedef uint64_t fe[5];

#define FE_MASK51 0x7ffffffffffffULL

void fe_0(fe h);
void fe_1(fe h);
void fe_copy(fe h, const fe f);
void fe_add(fe h, const fe f, const fe g);
void fe_sub(fe h, const fe f, const fe g);
void fe_mul(fe h, const fe f, const fe g);
void fe_sq(fe h, const fe f);
void fe_mul121666(fe h, const fe f);

/* Constant-time conditional swap: `swap` is 0 or 1 and is a SECRET (a scalar
 * bit). An `if (swap)` here would be correct and would pass every vector. */
void fe_cswap(fe a, fe b, uint64_t swap);

void fe_invert(fe out, const fe z);
void fe_frombytes(fe h, const uint8_t s[32]);
void fe_tobytes(uint8_t s[32], const fe f);
