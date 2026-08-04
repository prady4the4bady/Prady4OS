/* kernel/aether/ags.h — Agent Goal Signing (DDR-814), NSI 79/80.
 *
 * Every agent goal record is signed by the kernel with the owner's Ed25519 key
 * before it can be acted on, so the goal set is verifiable OFFLINE by anyone
 * holding OWNER_SIGN_PUBKEY. That is the whole security property: a goal an
 * agent gave itself is indistinguishable from one the owner set, unless the
 * owner's signature is what separates them.
 *
 * WHY THE KERNEL SIGNS, not the agent: if a ring-3 agent held the signing key
 * it could authorise its own goals, and the audit log would record a valid
 * signature on a goal nobody approved. The key never leaves the kernel;
 * SYS_GOAL_SIGN is CAP_SOVEREIGN-gated (only the operator's process may ask for
 * a signature) while SYS_GOAL_VERIFY is open to CAP_AGENT — verification uses
 * only public data, and an agent SHOULD be able to check its own goals.
 *
 * That asymmetry is the inverse of ACC's (seal=agent, open=owner) and for the
 * same underlying reason: the privileged direction is whichever one an attacker
 * would want.
 */
#pragma once
#include <stdint.h>

#define AGS_HASH_LEN 32u        /* SHA-256 over the goal record */
#define AGS_SIG_LEN  64u

enum {
    AGS_OK        =  0,
    AGS_ERR_ARG   = -1,
    AGS_ERR_VERIFY= -2          /* signature does not match this goal+key */
};

/* sig = Ed25519(owner_seed, SHA-256(goal)). The record is hashed first so a
 * goal of any length costs one fixed-size signature, and so the audit log can
 * store the 32-byte hash rather than the whole record. */
int ags_sign(uint8_t sig[AGS_SIG_LEN],
             const void *goal, uint32_t goallen,
             const uint8_t owner_sign_seed[32]);

/* 0 == the signature is the owner's over exactly this goal. Any non-zero means
 * REJECT — callers must not act on the goal. */
int ags_verify(const uint8_t sig[AGS_SIG_LEN],
               const void *goal, uint32_t goallen,
               const uint8_t owner_sign_pub[32]);
