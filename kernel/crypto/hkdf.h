/* kernel/crypto/hkdf.h — HMAC-SHA256 and HKDF-SHA256 (RFC 2104 / RFC 5869),
 * DDR-818.
 *
 * ACC (DDR-813) derives two independent AEAD keys from one X25519 shared secret,
 * separated only by the info label:
 *
 *   K_session = HKDF(shared, channel_id || "acc-v1")
 *   K_owner   = HKDF(shared, channel_id || "owner-cc-v1")
 *
 * The label separation is what makes K_session != K_owner, which is in turn what
 * lets the same nonce appear in both boxes without that being a break. So this
 * is not a convenience primitive — the envelope's safety argument rests on it.
 *
 * Same rules as DDR-811: pure C, no stdlib, no allocation, caller-provided
 * buffers, portable across x86_64/aarch64/riscv64.
 */
#pragma once
#include <stdint.h>
#include "sha256.h"

/* RFC 2104. `out` is 32 bytes. Keys longer than the 64-byte block are hashed
 * first, shorter ones zero-padded — both handled internally. */
void hmac_sha256(const void *key, uint32_t klen,
                 const void *msg, uint32_t mlen,
                 uint8_t out[SHA256_DIGEST_LEN]);

/* RFC 5869 extract-then-expand. `salt` may be NULL, which per the RFC means a
 * string of HashLen ZERO BYTES — not a zero-length string. That distinction is
 * a real source of interop bugs and is covered by vector 3.
 *
 * Returns 0, or <0 if okmlen exceeds 255*32 (the construction's maximum). A
 * caller asking for more key material than HKDF can produce has a bug, and
 * silently truncating would hide it. */
#define HKDF_MAX_OKM (255u * SHA256_DIGEST_LEN)

int hkdf_sha256(const void *salt, uint32_t saltlen,
                const void *ikm,  uint32_t ikmlen,
                const void *info, uint32_t infolen,
                uint8_t *okm,     uint32_t okmlen);
