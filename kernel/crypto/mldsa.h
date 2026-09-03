/* kernel/crypto/mldsa.h — ML-DSA-44 (FIPS 204) key generation, DDR-1054.
 *
 * Step 2 of Post-Quantum Security. Built on the Keccak core from DDR-1052:
 * ML-DSA uses SHAKE128 for matrix expansion and SHAKE256 for secret sampling
 * and hashing, which is why Keccak had to land first.
 *
 * KEYGEN ONLY, and deliberately so. keyGen is DETERMINISTIC -- a 32-byte seed
 * maps to exactly one (pk, sk) -- so it can be checked against NIST's ACVP
 * vectors byte-exactly, which is the only way to show FIPS 204 conformance. A
 * sign-then-verify gate would pass on any self-consistent wrong implementation
 * (checklist §5.1b.1 fact 4). Signing is a separate, later step.
 *
 * SCRATCH IS CALLER-OWNED. The working set is ~10 KiB (the NTT'd secret vector,
 * the accumulator, one expanded matrix polynomial at a time). That does not
 * belong on a kernel stack, and hiding it in file-scope statics would make the
 * routine non-reentrant, so the caller supplies it and decides where it lives.
 */
#pragma once
#include <stdint.h>

#define MLDSA44_SEED_BYTES   32u
#define MLDSA44_PK_BYTES   1312u   /* 32 + 4*320                              */
#define MLDSA44_SK_BYTES   2560u   /* 32+32+64 + 4*96 + 4*96 + 4*416          */

#define MLDSA_N 256
#define MLDSA_K 4
#define MLDSA_L 4

typedef struct {
    int32_t s1hat[MLDSA_L][MLDSA_N];   /* NTT(s1)                             */
    int32_t acc[MLDSA_N];              /* one row of A*s1, in the NTT domain  */
    int32_t aij[MLDSA_N];              /* one expanded A[r][s], on the fly    */
    int8_t  s1[MLDSA_L][MLDSA_N];      /* centered secrets, |c| <= eta        */
    int8_t  s2[MLDSA_K][MLDSA_N];
    int32_t t1[MLDSA_K][MLDSA_N];
    int32_t t0[MLDSA_K][MLDSA_N];
} mldsa44_scratch;

/* seed -> (pk, sk). Returns 0. Deterministic; no RNG is consulted. */
int mldsa44_keygen(const uint8_t seed[MLDSA44_SEED_BYTES],
                   uint8_t pk[MLDSA44_PK_BYTES],
                   uint8_t sk[MLDSA44_SK_BYTES],
                   mldsa44_scratch *scratch);

/* Known-answer self-test against the pinned ACVP vectors (mldsa_kat.h).
 * Returns 0, or the 1-based index of the first failing vector. */
int mldsa44_selftest(mldsa44_scratch *scratch);
