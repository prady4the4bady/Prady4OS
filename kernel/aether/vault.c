/* kernel/aether/vault.c — secure credential vault (DDR-834, table 6.9).
 *
 * See vault.h for why the record name is the AEAD's additional authenticated
 * data. Everything here is one fixed-size table written whole; the vault is
 * small and rewriting it entirely avoids a partial-update window in which the
 * file is neither the old nor the new state.
 */
#include "vault.h"
#include "hkdf.h"
#include "aead.h"
#include "rng.h"
#include "vfs.h"
#include "cap.h"
#include "string.h"
#include "console.h"

#define VAULT_PATH "/VAULT.BIN"
#define VAULT_INFO "PRADYOS-VAULT-v1"

/* Derived, never stored. The vault key exists only for the duration of a call —
 * a key kept in a global would outlive the operation that needed it and widen
 * what a kernel memory disclosure yields. */
static int vault_key(uint8_t k[AEAD_KEY_LEN], const uint8_t owner_seed[32]) {
    return hkdf_sha256(0, 0, owner_seed, 32,
                       VAULT_INFO, (uint32_t)sizeof(VAULT_INFO) - 1,
                       k, AEAD_KEY_LEN);
}

static int name_eq(const uint8_t a[VAULT_MAX_NAME], const uint8_t b[VAULT_MAX_NAME]) {
    for (unsigned i = 0; i < VAULT_MAX_NAME; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static int name_is_free(const uint8_t a[VAULT_MAX_NAME]) {
    for (unsigned i = 0; i < VAULT_MAX_NAME; i++)
        if (a[i])
            return 0;
    return 1;
}

/* Pack a caller name into the fixed field, NUL-padded. Returns 0 on success, or
 * -1 if it does not fit — a silently truncated name would collide two different
 * credentials into one record. */
static int name_pack(uint8_t out[VAULT_MAX_NAME], const char *name) {
    unsigned i = 0;
    if (!name || !name[0])
        return -1;
    for (; name[i]; i++) {
        if (i >= VAULT_MAX_NAME)
            return -1;
        out[i] = (uint8_t)name[i];
    }
    for (; i < VAULT_MAX_NAME; i++)
        out[i] = 0;
    return 0;
}

/* Load the whole table. A missing file is an EMPTY vault, not an error: the
 * first put has to be able to create it. */
static int vault_load(cap_t cap, int mnt, vault_rec_t *tab) {
    memset(tab, 0, sizeof(vault_rec_t) * VAULT_MAX_RECORDS);

    struct vfs_file f;
    if (vfs_open(cap, mnt, VAULT_PATH, &f) != 0)
        return VAULT_OK;                       /* absent == empty */

    uint32_t want = (uint32_t)(sizeof(vault_rec_t) * VAULT_MAX_RECORDS);
    if (f.size < want) {
        kputs("[vault] load: file exists but size="); kputdec(f.size);
        kputs(" want="); kputdec((uint64_t)want);
        kputs("\r\n");
        return VAULT_ERR_IO;                   /* truncated: refuse to guess */
    }
    /* vfs_read/vfs_write return the BYTE COUNT, not 0. Comparing against 0
     * treats a fully successful transfer as a failure — which is exactly what
     * the first version of this file did, and it cost a gate run to find. */
    int rd = vfs_read(cap, &f, 0, tab, want);
    if (rd != (int)want) {
        kputs("[vault] load: read rc="); kputdec((uint64_t)(int64_t)rd);
        kputs(" want="); kputdec((uint64_t)want);
        kputs("\r\n");
        return VAULT_ERR_IO;
    }
    return VAULT_OK;
}

static int vault_store(cap_t cap, int mnt, const vault_rec_t *tab) {
    struct vfs_file f;
    int oo = vfs_open(cap, mnt, VAULT_PATH, &f);
    int cc = 0;
    if (oo != 0) {
        cc = vfs_create(cap, mnt, VAULT_PATH, &f);
        if (cc != 0) {
            /* Which step failed, not just that one did. */
            kputs("[vault] store: open rc="); kputdec((uint64_t)(int64_t)oo);
            kputs(" create rc="); kputdec((uint64_t)(int64_t)cc);
            kputs(" mnt="); kputdec((uint64_t)(int64_t)mnt);
            kputs("\r\n");
            return VAULT_ERR_IO;
        }
    }

    uint32_t n = (uint32_t)(sizeof(vault_rec_t) * VAULT_MAX_RECORDS);
    /* Byte count, not 0 — see vault_load. The whole table is ~9.5 KiB, well
     * inside ADR-032's 1 MiB write-budget burst, so a short return here means a
     * real FS failure rather than rate limiting. */
    int wr = vfs_write(cap, &f, 0, tab, n);
    if (wr != (int)n) {
        kputs("[vault] store: write rc="); kputdec((uint64_t)(int64_t)wr);
        kputs(" want="); kputdec((uint64_t)n);
        kputs("\r\n");
        return VAULT_ERR_IO;
    }
    return VAULT_OK;
}

int vault_put(cap_t cap, int mnt,
              const char *name, const void *secret, uint32_t secretlen,
              const uint8_t owner_seed[32]) {
    if (!secret || !owner_seed || secretlen == 0 || secretlen > VAULT_MAX_SECRET)
        return VAULT_ERR_ARG;

    uint8_t nm[VAULT_MAX_NAME];
    if (name_pack(nm, name) != 0)
        return VAULT_ERR_ARG;

    uint8_t k[AEAD_KEY_LEN];
    if (vault_key(k, owner_seed) != 0)
        return VAULT_ERR_KEY;

    static vault_rec_t tab[VAULT_MAX_RECORDS];   /* kernel .bss, not ring-3 */
    int rc = vault_load(cap, mnt, tab);
    if (rc != VAULT_OK)
        return rc;

    /* Replace an existing record with this name, else take a free slot. The
     * vault is a map: two records under one name would make "which credential
     * is PROD_TOKEN?" depend on scan order. */
    vault_rec_t *slot = 0;
    for (unsigned i = 0; i < VAULT_MAX_RECORDS; i++) {
        if (!name_is_free(tab[i].name) && name_eq(tab[i].name, nm)) {
            slot = &tab[i];
            break;
        }
    }
    if (!slot) {
        for (unsigned i = 0; i < VAULT_MAX_RECORDS; i++) {
            if (name_is_free(tab[i].name)) { slot = &tab[i]; break; }
        }
    }
    if (!slot)
        return VAULT_ERR_NOSPC;                  /* never evict — DDR-834 */

    /* A fresh nonce per write. Reusing one across two secrets under the same key
     * is a ChaCha20 keystream reuse, which leaks the XOR of both plaintexts. */
    if (rng_bytes(slot->nonce, VAULT_NONCE_LEN) != 0)
        return VAULT_ERR_RNG;

    memcpy(slot->name, nm, VAULT_MAX_NAME);
    slot->ctlen = secretlen;
    aead_seal(k, slot->nonce, nm, VAULT_MAX_NAME,
              secret, secretlen, slot->ct, slot->tag);

    return vault_store(cap, mnt, tab);
}

int vault_get(cap_t cap, int mnt,
              const char *name, void *out, uint32_t *outlen,
              const uint8_t owner_seed[32]) {
    if (!out || !outlen || !owner_seed)
        return VAULT_ERR_ARG;

    uint8_t nm[VAULT_MAX_NAME];
    if (name_pack(nm, name) != 0)
        return VAULT_ERR_ARG;

    uint8_t k[AEAD_KEY_LEN];
    if (vault_key(k, owner_seed) != 0)
        return VAULT_ERR_KEY;

    static vault_rec_t tab[VAULT_MAX_RECORDS];
    int rc = vault_load(cap, mnt, tab);
    if (rc != VAULT_OK)
        return rc;

    for (unsigned i = 0; i < VAULT_MAX_RECORDS; i++) {
        if (name_is_free(tab[i].name) || !name_eq(tab[i].name, nm))
            continue;
        if (tab[i].ctlen == 0 || tab[i].ctlen > VAULT_MAX_SECRET)
            return VAULT_ERR_AUTH;               /* length itself is suspect */

        /* aead_open verifies the tag BEFORE writing any plaintext, so a
         * tampered record yields nothing rather than "the data, but flagged". */
        if (aead_open(k, tab[i].nonce, nm, VAULT_MAX_NAME,
                      tab[i].ct, tab[i].ctlen, tab[i].tag, out) != 0)
            return VAULT_ERR_AUTH;

        *outlen = tab[i].ctlen;
        return VAULT_OK;
    }
    return VAULT_ERR_NOENT;
}
