/* kernel/crypto/keccak.h — Keccak-f[1600], SHA-3 and SHAKE (FIPS 202), DDR-1052.
 *
 * WHY THIS EXISTS. Post-quantum cryptography is mandatory v1 scope (operator,
 * PR #17 Part B): ML-KEM (FIPS 203) and ML-DSA (FIPS 204). BOTH are built on
 * SHAKE128/256 for matrix expansion, coefficient sampling and hashing — so a
 * Keccak core is a PREREQUISITE for either, not an optional extra. There was no
 * SHA-3 anywhere in this tree: kernel/crypto/ held SHA-256, SHA-512, X25519,
 * Ed25519, HKDF and AEAD, and zero Keccak.
 *
 * Pure C, no hardware acceleration, no stdlib, no allocation — the same reason
 * sha256.h gives: one object builds for x86_64, aarch64 and riscv64, and an
 * ISA-specific path would pass its gate on one and fail at runtime on the
 * others, which is the worst failure shape available.
 *
 * ENDIAN-NEUTRAL BY CONSTRUCTION. Bytes are XORed into (and read out of) the
 * lane array with explicit shifts rather than by aliasing the state as bytes, so
 * the FIPS 202 little-endian lane convention holds regardless of host byte
 * order. No __builtin_bswap, no #ifdef.
 *
 * Streaming API: init -> update (any number of times, any sizes) -> squeeze.
 * The caller owns the context and it lives on the stack; call sites include the
 * audit-ledger path, where the allocator is not guaranteed.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define KECCAK_RATE_SHAKE128 168u
#define KECCAK_RATE_SHAKE256 136u
#define KECCAK_RATE_SHA3_256 136u
#define KECCAK_RATE_SHA3_512  72u

typedef struct {
    uint64_t st[25];        /* the 1600-bit state, as 25 little-endian lanes  */
    unsigned rate;          /* bytes absorbed/squeezed per permutation        */
    unsigned pos;           /* offset within the current rate block           */
    uint8_t  dom;           /* domain separation: 0x1F for XOF, 0x06 for SHA-3*/
    uint8_t  squeezing;     /* 0 while absorbing; padding applied on the flip */
} keccak_ctx;

void keccak_init(keccak_ctx *c, unsigned rate, uint8_t dom);
void keccak_update(keccak_ctx *c, const uint8_t *in, size_t len);
/* May be called repeatedly; output continues where the last call stopped. */
void keccak_squeeze(keccak_ctx *c, uint8_t *out, size_t len);

static inline void shake128_init(keccak_ctx *c) { keccak_init(c, KECCAK_RATE_SHAKE128, 0x1F); }
static inline void shake256_init(keccak_ctx *c) { keccak_init(c, KECCAK_RATE_SHAKE256, 0x1F); }

/* One-shot helpers. SHA-3 uses domain 0x06; the digest length is fixed by the
 * rate, so these are the only two fixed-length variants offered. */
void shake128(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen);
void shake256(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen);
void sha3_256(const uint8_t *in, size_t inlen, uint8_t out[32]);
void sha3_512(const uint8_t *in, size_t inlen, uint8_t out[64]);

/* Known-answer self-test. Returns 0 on success, or the 1-based index of the
 * first failing vector. Vectors are pinned in keccak_kat.h. */
int keccak_selftest(void);
