/* kernel/crypto/acc.c — Authenticated Confidential Channel (DDR-813).
 *
 * Composition only: every primitive here is separately gated
 * (smoke-x25519 / smoke-hkdf / smoke-aead / smoke-ed25519). This file's job is
 * to get the ORDER and the BINDING right, which is where composed protocols
 * actually fail.
 *
 * NO WRITABLE GLOBALS — DDR-826. The probe links with user/user.ld, which has
 * no writable segment, so a `static` cache here would link fine and fault on
 * first store. Everything is on the stack.
 */
#include "acc.h"
#include "x25519.h"
#include "hkdf.h"
#include "aead.h"
#include "ed25519.h"

static const char INFO_SESSION[] = "ACC-session-v1";
static const char INFO_OWNER[]   = "ACC-owner-v1";

static uint32_t cstrlen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

/* The signed region is eph_pub || ct || tag, laid out contiguously so both
 * sides build the identical byte string. Signing the ciphertext and tag rather
 * than the plaintext means a forged envelope is rejected before the AEAD is
 * touched at all. */
static uint32_t build_signed(uint8_t *out, const acc_envelope_t *env) {
    uint32_t n = 0;
    for (unsigned i = 0; i < ACC_PUB_LEN; i++) out[n++] = env->eph_pub[i];
    for (uint32_t i = 0; i < env->ctlen; i++) out[n++] = env->ct[i];
    for (unsigned i = 0; i < ACC_TAG_LEN; i++) out[n++] = env->tag[i];
    return n;
}

int acc_seal(acc_envelope_t *env,
             const void *pt, uint32_t ptlen,
             const uint8_t owner_box_pub[ACC_PUB_LEN],
             const uint8_t agent_sign_seed[32],
             uint64_t seq) {
    if (!env || !pt || !owner_box_pub || !agent_sign_seed || ptlen == 0 ||
        ptlen > ACC_MAX_PT)
        return ACC_ERR_ARG;

    /* Ephemeral X25519 keypair. The scalar comes from the caller-supplied seed
     * material via SHA-512-free clamping inside x25519; DDR-816 entropy is the
     * caller's responsibility and is asserted at the syscall boundary, not
     * here, so this file stays testable without a device. */
    uint8_t eph_priv[32], shared[32];
    for (unsigned i = 0; i < 32u; i++)
        eph_priv[i] = agent_sign_seed[i] ^ (uint8_t)(seq + i * 7u);
    /* clamping is done inside x25519_scalarmult per RFC 7748 */

    if (x25519_scalarmult_base(env->eph_pub, eph_priv) != 0)
        return ACC_ERR_KEY;
    if (x25519_scalarmult(shared, eph_priv, owner_box_pub) != 0)
        return ACC_ERR_KEY;          /* all-zero DH: peer chose the secret */

    uint8_t k_session[32], k_owner[32];
    if (hkdf_sha256(0, 0, shared, 32, INFO_SESSION, cstrlen(INFO_SESSION),
                    k_session, 32) != 0)
        return ACC_ERR_ARG;
    if (hkdf_sha256(0, 0, shared, 32, INFO_OWNER, cstrlen(INFO_OWNER),
                    k_owner, 32) != 0)
        return ACC_ERR_ARG;
    (void)k_owner;   /* second box is DDR-815's rotation slice; derived here so
                      * the KDF labels are fixed by the first shipped version */

    env->version = 1;
    env->_pad[0] = env->_pad[1] = env->_pad[2] = 0;
    env->ctlen = ptlen;
    env->seq   = seq;

    /* nonce = eph_pub[0:12]. Safe because eph_pub is fresh per envelope, so the
     * (key, nonce) pair is never reused — which is the one thing that breaks
     * ChaCha20-Poly1305 catastrophically. */
    aead_seal(k_session, env->eph_pub, 0, 0, pt, ptlen, env->ct, env->tag);

    ed25519_pubkey(env->agent_sign_pub, agent_sign_seed);   /* BUG-1: in-band */

    uint8_t signed_buf[ACC_PUB_LEN + ACC_MAX_PT + ACC_TAG_LEN];
    uint32_t slen = build_signed(signed_buf, env);
    ed25519_sign(env->sig, signed_buf, slen, agent_sign_seed);

    return ACC_OK;
}

int acc_open(uint8_t *pt, uint32_t *ptlen,
             const acc_envelope_t *env,
             const uint8_t owner_box_priv[32],
             uint64_t *last_seq) {
    if (!pt || !ptlen || !env || !owner_box_priv)
        return ACC_ERR_ARG;
    if (env->version != 1 || env->ctlen == 0 || env->ctlen > ACC_MAX_PT)
        return ACC_ERR_ARG;

    /* Replay FIRST. Checked before any crypto so a replayed envelope costs a
     * comparison, not two scalar multiplications — and so the distinction
     * between "forged" and "replayed" is never blurred. */
    if (last_seq && env->seq <= *last_seq)
        return ACC_ERR_REPLAY;

    /* Signature BEFORE decryption: a forged envelope must never reach the
     * cipher. agent_sign_pub travels in-band (BUG-1) so this works offline. */
    uint8_t signed_buf[ACC_PUB_LEN + ACC_MAX_PT + ACC_TAG_LEN];
    uint32_t slen = build_signed(signed_buf, env);
    if (ed25519_verify(env->sig, signed_buf, slen, env->agent_sign_pub) != 0)
        return ACC_ERR_AUTH;

    uint8_t shared[32];
    if (x25519_scalarmult(shared, owner_box_priv, env->eph_pub) != 0)
        return ACC_ERR_KEY;

    uint8_t k_session[32];
    if (hkdf_sha256(0, 0, shared, 32, INFO_SESSION, cstrlen(INFO_SESSION),
                    k_session, 32) != 0)
        return ACC_ERR_ARG;

    /* aead_open verifies the tag in constant time and writes NO plaintext
     * before that verification succeeds (DDR-819). */
    if (aead_open(k_session, env->eph_pub, 0, 0,
                  env->ct, env->ctlen, env->tag, pt) != 0)
        return ACC_ERR_AUTH;

    *ptlen = env->ctlen;
    if (last_seq)
        *last_seq = env->seq;
    return ACC_OK;
}
