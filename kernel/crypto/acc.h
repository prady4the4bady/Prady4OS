/* kernel/crypto/acc.h — Authenticated Confidential Channel (DDR-813).
 *
 * The envelope an agent uses to hand the owner data that the owner must be able
 * to read LATER, OFFLINE, AFTER A REBOOT — while nobody else can read it and
 * nobody can forge it.
 *
 * Composition, all five primitives now gated:
 *
 *   eph_priv, eph_pub  <- X25519 keypair, fresh per envelope   (DDR-820)
 *   shared             <- X25519(eph_priv, peer_box_pub)       (DDR-820)
 *   K_session          <- HKDF-SHA256(shared, salt=0, "ACC-session-v1")  (818)
 *   K_owner            <- HKDF-SHA256(shared, salt=0, "ACC-owner-v1")    (818)
 *   ct, tag            <- ChaCha20-Poly1305(K_session, eph_pub[0:12], pt) (819)
 *   sig                <- Ed25519(agent_sign_priv, eph_pub||ct||tag)      (821)
 *
 * TWO SPEC BUGS WERE FIXED AT DESIGN TIME, both recorded because both are the
 * kind that only surface long after they are introduced:
 *
 * BUG-1 — agent_pubkey[32] IS IN THE ENVELOPE. Without it the owner cannot
 *   verify the signature after a reboot: the ephemeral key is gone and there is
 *   nothing binding the envelope to an agent. The whole point of ACC is the
 *   offline read, so an envelope the owner cannot check is an envelope that
 *   fails exactly when it is needed.
 *
 * BUG-2 — THE ED25519 KEY AND THE X25519 KEY ARE DISTINCT FIELDS. Using one key
 *   for both signing and key agreement is a protocol error, not an
 *   optimisation: the two schemes interpret the same 32 bytes as different
 *   objects (an Edwards point vs a Montgomery u-coordinate), and sharing them
 *   invites cross-protocol attacks. `owner_box_pub` (X25519) and
 *   `agent_sign_pub` (Ed25519) never alias.
 */
#pragma once
#include <stdint.h>

#define ACC_PUB_LEN    32u
#define ACC_TAG_LEN    16u
#define ACC_SIG_LEN    64u
#define ACC_MAX_PT     512u      /* bounded: S2 — every buffer has a ceiling */

/* Wire format. Fixed layout, no length-prefixed fields, so a truncated or
 * over-long envelope is a size mismatch rather than a parse. */
typedef struct {
    uint8_t  version;                    /* 1; a future format bumps this      */
    uint8_t  _pad[3];
    uint32_t ctlen;                      /* == plaintext length, <= ACC_MAX_PT */
    uint64_t seq;                        /* replay: strictly increasing        */
    uint8_t  eph_pub[ACC_PUB_LEN];       /* X25519 ephemeral public            */
    uint8_t  agent_sign_pub[ACC_PUB_LEN];/* BUG-1: Ed25519 verify key, in-band */
    uint8_t  tag[ACC_TAG_LEN];
    uint8_t  sig[ACC_SIG_LEN];
    uint8_t  ct[ACC_MAX_PT];
} acc_envelope_t;

enum {
    ACC_OK          =  0,
    ACC_ERR_ARG     = -1,   /* bad length / null / version                     */
    ACC_ERR_AUTH    = -2,   /* signature or AEAD tag rejected                  */
    ACC_ERR_REPLAY  = -3,   /* seq not strictly greater than the last accepted */
    ACC_ERR_KEY     = -4    /* peer key rejected (small order / all-zero DH)   */
};

/* Seal `pt` for the holder of `owner_box_pub`, signed by `agent_sign_seed`.
 * Returns ACC_OK or a negative ACC_ERR_*. `eph_priv` comes from DDR-816
 * entropy; ACC refuses to run if rng_bytes() fails rather than using a
 * predictable scalar. */
int acc_seal(acc_envelope_t *env,
             const void *pt, uint32_t ptlen,
             const uint8_t owner_box_pub[ACC_PUB_LEN],
             const uint8_t agent_sign_seed[32],
             uint64_t seq);

/* Open with the owner's X25519 private key. Verifies the Ed25519 signature
 * BEFORE attempting decryption, and the AEAD tag before writing any plaintext,
 * so a forged envelope never reaches the cipher and never yields bytes.
 *
 * `last_seq` is the highest sequence accepted so far on this channel; an
 * envelope with seq <= last_seq is ACC_ERR_REPLAY. On success `*last_seq` is
 * advanced. Pass NULL to skip the replay check (offline owner reads of an
 * archived envelope, where ordering is not being enforced). */
int acc_open(uint8_t *pt, uint32_t *ptlen,
             const acc_envelope_t *env,
             const uint8_t owner_box_priv[32],
             uint64_t *last_seq);
