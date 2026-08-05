/* kernel/aether/vault.h — secure credential vault (DDR-834, table 6.9).
 *
 * Cloud-bridge credentials encrypted at rest in SFS. A credential in a plaintext
 * file is a credential that leaks with the disk, and this project's images are
 * routinely copied and attached to QEMU.
 *
 *   K_vault <- HKDF-SHA256(owner_seed, salt=0, "PRADYOS-VAULT-v1")
 *   ct, tag <- ChaCha20-Poly1305(K_vault, nonce, secret, aad = name)
 *
 * THE NAME IS THE AAD, and that is not decoration. Without it a record could be
 * RENAMED on disk — so that a request for PROD_TOKEN returns the secret stored
 * for TEST_TOKEN — while every tag still verified. Confidentiality of the values
 * is not integrity of the association; binding the name into the tag makes the
 * mapping part of what is authenticated.
 */
#pragma once
#include <stdint.h>
#include "cap.h"    /* the vault uses the CALLER's FS authority, not its own */

#define VAULT_MAX_NAME    16u    /* S2: every buffer has a ceiling */
#define VAULT_MAX_SECRET  256u
#define VAULT_MAX_RECORDS 32u
#define VAULT_NONCE_LEN   12u
#define VAULT_TAG_LEN     16u

enum {
    VAULT_OK        =  0,
    VAULT_ERR_ARG   = -1,   /* null / oversized name or secret          */
    VAULT_ERR_AUTH  = -2,   /* tag rejected: tampered or wrong key      */
    VAULT_ERR_NOENT = -3,   /* no record under that name                */
    VAULT_ERR_NOSPC = -4,   /* table full — never evicts, see DDR-834   */
    VAULT_ERR_IO    = -5,   /* SFS read/write failed                    */
    /* Split out from IO deliberately: "the disk refused" and "there was no
     * entropy" send you to different subsystems, and one errno for both wastes
     * a debugging cycle every time. */
    VAULT_ERR_RNG   = -6,   /* no entropy: refuse rather than reuse a nonce */
    VAULT_ERR_KEY   = -7    /* HKDF failed deriving K_vault                 */
};

/* On-disk record. Fixed layout; a short or over-long file is a size mismatch
 * rather than a parse. */
typedef struct {
    uint8_t  name[VAULT_MAX_NAME];      /* NUL-padded; all-zero == free slot */
    uint8_t  nonce[VAULT_NONCE_LEN];
    uint8_t  tag[VAULT_TAG_LEN];
    uint32_t ctlen;
    uint8_t  ct[VAULT_MAX_SECRET];
} vault_rec_t;

/* Store `secret` under `name`, replacing any existing record with that name —
 * the vault is a map, not a log. Returns VAULT_OK or a negative VAULT_ERR_*. */
int vault_put(cap_t cap, int mnt,
              const char *name, const void *secret, uint32_t secretlen,
              const uint8_t owner_seed[32]);

/* Fetch the secret stored under `name`. Writes the plaintext length to *outlen.
 * A missing name is VAULT_ERR_NOENT, never a zero-length success: handing back
 * an empty "credential" would let the caller authenticate with nothing and
 * discover the problem somewhere far away. */
int vault_get(cap_t cap, int mnt,
              const char *name, void *out, uint32_t *outlen,
              const uint8_t owner_seed[32]);
