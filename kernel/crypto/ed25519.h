/* kernel/crypto/ed25519.h — Ed25519 signatures (RFC 8032), DDR-821.
 *
 * AGS signs every agent action record so the audit log can be verified OFFLINE
 * by anyone holding OWNER_SIGN_PUBKEY. That is the whole security property: a
 * log is only worth something if a holder of the log cannot forge entries in it.
 *
 * Deterministic per RFC 8032 — no RNG at signing time. That is a property of the
 * scheme, not a shortcut: the nonce is SHA-512(prefix || message), so a broken
 * RNG cannot leak the key the way it does in ECDSA.
 *
 * CONSTANT-TIME BY CONSTRUCTION, AND UNVERIFIED AS SUCH — owner decision D-1.
 * Signing must not leak the secret scalar or the nonce; a leaked nonce recovers
 * the private key from a SINGLE signature, which is strictly worse than X25519's
 * exposure. No branch and no memory index in ed25519.c depends on a secret.
 * But QEMU under TCG does not model cache or pipeline timing, so smoke-ed25519
 * green means "the vectors pass and the A/B discriminates" and says nothing
 * about side channels. Production use requires review with an instrument that
 * can measure it.
 *
 * ed25519_verify handles only public data and is deliberately NOT written to be
 * constant-time — stated so nobody "hardens" it and slows verification for no
 * security gain.
 *
 * Pure C, no stdlib, no allocation, no hardware acceleration (ADR-034).
 */
#pragma once
#include <stdint.h>

#define ED25519_SEED_LEN 32u
#define ED25519_PUB_LEN  32u
#define ED25519_SIG_LEN  64u

/* pubkey A = [s]B, where s is clamped from SHA-512(seed)[0..31]. */
void ed25519_pubkey(uint8_t pub[ED25519_PUB_LEN],
                    const uint8_t seed[ED25519_SEED_LEN]);

/* sig = R || S. `seed` is the 32-byte private seed; the public key is derived
 * internally rather than trusted from a caller-supplied pair, so a caller
 * cannot induce a mismatched (seed, pubkey) signature. */
void ed25519_sign(uint8_t sig[ED25519_SIG_LEN],
                  const void *msg, uint32_t msglen,
                  const uint8_t seed[ED25519_SEED_LEN]);

/* 0 == valid signature. Any non-zero == REJECTED. Callers must treat non-zero
 * as forgery, never as a transient error. */
int  ed25519_verify(const uint8_t sig[ED25519_SIG_LEN],
                    const void *msg, uint32_t msglen,
                    const uint8_t pub[ED25519_PUB_LEN]);
