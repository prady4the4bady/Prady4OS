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

/* ---- signing (DDR-1057) ------------------------------------------------
 *
 * ML-DSA.Sign_internal (FIPS 204 Alg. 7), DETERMINISTIC variant: rnd is 32 zero
 * bytes, so one (sk, message) maps to exactly one signature and the answer is an
 * ACVP constant. FIPS 204's default is a RANDOM rnd, and a randomized signature
 * can only be checked by verifying it -- which passes on any self-consistent
 * wrong implementation. Determinism is what makes this testable, and it is also
 * what a reproducible audit ledger wants.
 *
 * Scratch is caller-owned for the same two reasons as keyGen's: reentrancy, and
 * a ring-3 probe that must have NO writable allocated section (DDR-826). ~54 KiB
 * -- larger than the 32 KiB ADR-038 maps eagerly, but the user stack is 8 MiB
 * and demand-paged, so the frame simply faults itself in. */
#define MLDSA44_SIG_BYTES 2420u

typedef struct {
    uint32_t zetas[256];
    uint32_t ninv;
    int      tables_ready;

    int32_t s1h[MLDSA_L][MLDSA_N], s2h[MLDSA_K][MLDSA_N], t0h[MLDSA_K][MLDSA_N];
    int32_t y[MLDSA_L][MLDSA_N],  yh[MLDSA_L][MLDSA_N];
    int32_t w[MLDSA_K][MLDSA_N],  w1[MLDSA_K][MLDSA_N];
    int32_t cs1[MLDSA_L][MLDSA_N], cs2[MLDSA_K][MLDSA_N], ct0[MLDSA_K][MLDSA_N];
    int32_t z[MLDSA_L][MLDSA_N];
    int32_t c[MLDSA_N], ch[MLDSA_N], aij[MLDSA_N], acc[MLDSA_N];
    uint8_t hint[MLDSA_K][MLDSA_N];
    uint8_t w1enc[MLDSA_K * 192];
    uint8_t sig[MLDSA44_SIG_BYTES];
} mldsa44_sign_scratch;

/* Returns 0 on success, or -1 if the rejection loop exceeded its bound.
 * The bound is not decoration: FIPS 204 states no iteration cap, the expected
 * count for ML-DSA-44 is about 4.25, and an unbounded loop in this codebase is
 * an S2 violation (DDR-961/994). */
int mldsa44_sign_internal(const uint8_t sk[MLDSA44_SK_BYTES],
                          const uint8_t *msg, unsigned long msglen,
                          uint8_t sig[MLDSA44_SIG_BYTES],
                          mldsa44_sign_scratch *scratch);

/* Known-answer self-test against the pinned deterministic ACVP Sign_internal
 * vectors (mldsa_sig_kat.h). Returns 0, or the 1-based failing vector index. */
int mldsa44_sign_selftest(mldsa44_sign_scratch *scratch);
unsigned mldsa44_sig_kat_count(void);
unsigned mldsa44_decomp_count(void);

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
