/* kernel/crypto/x25519.h — X25519 key agreement (RFC 7748), DDR-820.
 *
 * ACC agrees a secret with a peer, feeds it to DDR-818's HKDF to derive
 * K_session and K_owner, and encrypts with DDR-819's AEAD. This is the
 * agreement.
 *
 * CONSTANT-TIME BY CONSTRUCTION, AND UNVERIFIED AS SUCH. No branch and no
 * memory index in x25519.c depends on a secret bit; the ladder's conditional
 * swap is an arithmetic mask, never an `if`. But an implementation can pass
 * every RFC 7748 vector perfectly and still leak the private key through timing
 * or cache behaviour, and NO GATE THIS PROJECT CAN RUN DETECTS THAT — QEMU under
 * TCG does not model cache or pipeline timing. `smoke-x25519` green means the
 * vectors pass and the A/B discriminates, and says nothing about timing-channel
 * resistance. Production use requires side-channel review with an instrument
 * that can measure it (dudect, ctgrind, or a verified implementation such as
 * fiat-crypto). Accepted on the record by the owner; see DDR-820.
 *
 * No stdlib, no allocation, no hardware acceleration: the same source builds
 * for x86_64, aarch64 and riscv64 (ADR-034). It does require `unsigned __int128`,
 * which all three provide — see DDR-820 for why the 5x51 layout was chosen over
 * the 32-bit-friendly 10-limb one.
 */
#pragma once
#include <stdint.h>

#define X25519_LEN 32u

/* out = scalar * point.
 *
 * Returns 0 on success, or <0 when the result is all-zero. That check is not
 * decoration: an all-zero shared secret means the peer CHOSE it by sending a
 * small-order point rather than agreeing to it, and every key derived from it
 * would be one the peer already knows. Callers must treat <0 as a failed
 * handshake, never as a usable secret. */
int x25519_scalarmult(uint8_t out[X25519_LEN],
                      const uint8_t scalar[X25519_LEN],
                      const uint8_t point[X25519_LEN]);

/* out = scalar * basepoint (u = 9) — the public-key half. Same return contract,
 * though a valid clamped scalar cannot produce zero here. */
int x25519_scalarmult_base(uint8_t out[X25519_LEN],
                           const uint8_t scalar[X25519_LEN]);
