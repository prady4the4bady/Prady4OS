/* kernel/crypto/sha256.h — SHA-256 (FIPS 180-4), DDR-811.
 *
 * Pure C, no hardware acceleration, no stdlib, no allocation. The same object
 * builds for x86_64, aarch64 and riscv64 — a SHA-NI/AES-NI path would compile
 * and pass its gate on x86_64 and fail at runtime on the other two, which is the
 * worst failure shape available: the gate that should catch it is the one that
 * passes.
 *
 * Streaming three-call API so a caller can hash a 4 KiB record or a whole kernel
 * image without buffering it. The caller owns the context — it lives on the
 * stack, because call sites include early boot and the shutdown path where the
 * allocator is not guaranteed.
 */
#pragma once
#include <stdint.h>

#define SHA256_DIGEST_LEN 32u
#define SHA256_BLOCK_LEN  64u

typedef struct {
    uint32_t state[8];                  /* H0..H7                              */
    uint64_t bitlen;                    /* total message length in BITS        */
    uint32_t buflen;                    /* bytes currently in buf (< 64)       */
    uint8_t  buf[SHA256_BLOCK_LEN];     /* partial block carried across update */
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, uint64_t len);
void sha256_final(sha256_ctx *c, uint8_t out[SHA256_DIGEST_LEN]);

/* One-shot convenience for the common case. */
void sha256(const void *data, uint64_t len, uint8_t out[SHA256_DIGEST_LEN]);
