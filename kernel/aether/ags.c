/* kernel/aether/ags.c — Agent Goal Signing (DDR-814).
 *
 * Composition only: SHA-256 (DDR-811) then Ed25519 (DDR-821), both separately
 * gated. This file's job is to hash the right bytes and to fail closed.
 *
 * NO WRITABLE GLOBALS (DDR-826) — the probe links with user.ld, which has no
 * writable segment, so a `static` cache here would link and fault on first
 * store. Everything is on the stack.
 */
#include "ags.h"
#include "sha256.h"
#include "ed25519.h"

int ags_sign(uint8_t sig[AGS_SIG_LEN],
             const void *goal, uint32_t goallen,
             const uint8_t owner_sign_seed[32]) {
    if (!sig || !goal || goallen == 0 || !owner_sign_seed)
        return AGS_ERR_ARG;

    uint8_t h[AGS_HASH_LEN];
    sha256(goal, goallen, h);
    ed25519_sign(sig, h, AGS_HASH_LEN, owner_sign_seed);
    return AGS_OK;
}

int ags_verify(const uint8_t sig[AGS_SIG_LEN],
               const void *goal, uint32_t goallen,
               const uint8_t owner_sign_pub[32]) {
    if (!sig || !goal || goallen == 0 || !owner_sign_pub)
        return AGS_ERR_ARG;

    uint8_t h[AGS_HASH_LEN];
    sha256(goal, goallen, h);
    /* ed25519_verify already rejects a non-canonical S and a signature that is
     * self-consistent but not bound to this public key. Anything non-zero is a
     * rejection — never re-interpreted as a transient error. */
    return ed25519_verify(sig, h, AGS_HASH_LEN, owner_sign_pub) == 0
           ? AGS_OK : AGS_ERR_VERIFY;
}
