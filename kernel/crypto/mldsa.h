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
 * SCRATCH IS CALLER-OWNED, and it holds EVERYTHING mutable -- the derived
 * twiddle tables included. That does not belong on a kernel stack, and
 * file-scope statics would make the routine non-reentrant, which this header
 * said from the start; putting the tables in the struct is what actually
 * delivers that, rather than claiming it while keeping a lazily-filled global.
 *
 * It is also a hard requirement for the ring-3 gate: user/user.ld links each
 * probe as a SINGLE R+X PT_LOAD, so a probe with any writable allocated
 * section links successfully and then faults on its first store (DDR-826).
 * With no `static` mutable state anywhere in mldsa.c, the probe has none.
 *
 * SIZE: ~21 KiB. The earlier '~10 KiB' in this header was wrong even for the
 * fields it then listed (s1hat/t1/t0 are 4 KiB each on their own).
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
    /* Derived at first use from q and zeta=1753 -- never transcribed. */
    uint32_t zetas[256];               /* zeta^brv8(i) mod q                  */
    uint32_t ninv;                     /* 256^-1 mod q, the invNTT scale      */
    int      tables_ready;

    uint8_t  pk[MLDSA44_PK_BYTES];     /* selftest working buffers            */
    uint8_t  sk[MLDSA44_SK_BYTES];

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

/* Known-answer self-test. Returns 0; a POSITIVE n is the 1-based index of the
 * first failing ACVP vector (mldsa_kat.h); a NEGATIVE n is the 1-based index of
 * a failing Power2Round boundary case. The two are signed apart because the
 * KATs do not cover that boundary -- measured, not assumed: r0 == 2^(D-1)
 * occurs in 0 of the 2048 coefficients across both vectors (DDR-1054 §M3). */
int mldsa44_selftest(mldsa44_scratch *scratch);

/* How many of each kind of vector the selftest actually ran. Exposed so the
 * ring-3 probe can REPORT the numbers rather than print literals that could
 * drift from the tables -- a gate asserting a hard-coded count would keep
 * passing after the vector set shrank. */
unsigned mldsa44_kat_count(void);
unsigned mldsa44_p2r_count(void);
