/* kernel/crypto/aead.h — ChaCha20-Poly1305 AEAD (RFC 8439), DDR-819.
 *
 * ACC's envelope carries two boxes over the same plaintext, one to the session
 * key and one to the owner's key. AES-GCM was rejected: it must be correct and
 * constant-time on three architectures, two of which have no AES instructions,
 * and software AES is both slower and timing-variable on riscv64 and early
 * aarch64. ChaCha20 and Poly1305 are add/xor/rotate plus a 130-bit
 * multiply-accumulate — constant-time as a property of the algorithm rather than
 * of table lookups.
 *
 * Same rules as DDR-811/818: pure C, no stdlib, no allocation, caller-provided
 * buffers, portable across x86_64/aarch64/riscv64.
 */
#pragma once
#include <stdint.h>

#define AEAD_KEY_LEN   32u
#define AEAD_NONCE_LEN 12u
#define AEAD_TAG_LEN   16u

/* Encrypt-and-authenticate. `ct` must have room for ptlen bytes; `tag` for 16.
 * `ct` may alias `pt`. */
void aead_seal(const uint8_t key[AEAD_KEY_LEN],
               const uint8_t nonce[AEAD_NONCE_LEN],
               const void *aad, uint32_t aadlen,
               const void *pt,  uint32_t ptlen,
               uint8_t *ct, uint8_t tag[AEAD_TAG_LEN]);

/* Verify-then-decrypt. Returns 0 on success, <0 if authentication fails.
 *
 * TWO PROPERTIES THIS MUST HAVE, both easy to break while still passing every
 * published vector:
 *
 *  1. The tag is verified BEFORE any plaintext is written. Returning "the data,
 *     but flagged" lets a careless caller use unauthenticated bytes, which
 *     defeats the point of an AEAD.
 *  2. The tag comparison is CONSTANT TIME. An early-exit compare leaks the tag
 *     prefix and turns forgery from 2^128 work into 16 sequential guesses.
 *
 * Property 2 cannot be tested by any gate this project can run: a constant-time
 * compare and an early-exit one accept and reject exactly the same inputs, and
 * differ only in instruction timing on real hardware. It is enforced here by
 * construction and by review — see DDR-819, which records that limitation rather
 * than shipping a gate arm that pretends to test it. */
int  aead_open(const uint8_t key[AEAD_KEY_LEN],
               const uint8_t nonce[AEAD_NONCE_LEN],
               const void *aad, uint32_t aadlen,
               const void *ct,  uint32_t ctlen,
               const uint8_t tag[AEAD_TAG_LEN], uint8_t *pt);

/* Exposed for the RFC 8439 §2.4.2 / §2.5.2 vectors, which test the stream
 * cipher and the authenticator in isolation — a failure in either is then
 * unambiguous rather than "somewhere in the AEAD". */
void chacha20_stream(const uint8_t key[32], const uint8_t nonce[12],
                     uint32_t counter, const void *in, uint32_t len, uint8_t *out);
void poly1305_mac(const uint8_t key[32], const void *msg, uint32_t len,
                  uint8_t tag[AEAD_TAG_LEN]);
