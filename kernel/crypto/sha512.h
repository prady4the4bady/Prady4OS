/* kernel/crypto/sha512.h — SHA-512 (FIPS 180-4), DDR-821.
 *
 * Exists because Ed25519 is SPECIFIED over SHA-512 (RFC 8032): the private
 * scalar and the deterministic nonce both come from it. SHA-256 cannot be
 * substituted — the digest length is load-bearing in both derivations.
 *
 * Deliberately its own file rather than a parameterisation of sha256.c. The two
 * share the Merkle-Damgard shape and differ in every dimension that matters:
 * 64-bit words, 128-byte blocks, 80 rounds, a different constant table, four
 * different rotation triples, and a 128-bit length field. Templating over all
 * of that would add places to be wrong in the one primitive whose wrongness is
 * silent, and neither implementation would get anything for it.
 *
 * Pure C, no stdlib, no allocation, no hardware acceleration: the same source
 * builds for x86_64, aarch64 and riscv64 (ADR-034).
 */
#pragma once
#include <stdint.h>

#define SHA512_DIGEST_LEN 64u
#define SHA512_BLOCK_LEN  128u

typedef struct {
    uint64_t h[8];
    uint64_t len_lo;                    /* message length in bytes, low 64  */
    uint64_t len_hi;                    /* ... high 64. FIPS 180-4 specifies */
                                        /* a 128-bit length field; carrying  */
                                        /* only 64 would silently wrap at    */
                                        /* 2^64 bytes rather than being      */
                                        /* wrong in a detectable way.        */
    uint8_t  buf[SHA512_BLOCK_LEN];
    uint32_t buflen;
} sha512_ctx;

void sha512_init(sha512_ctx *c);
void sha512_update(sha512_ctx *c, const void *data, uint64_t len);
void sha512_final(sha512_ctx *c, uint8_t out[SHA512_DIGEST_LEN]);

/* One-shot convenience; identical result to init/update/final. */
void sha512(const void *data, uint64_t len, uint8_t out[SHA512_DIGEST_LEN]);
