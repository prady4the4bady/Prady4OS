/* kernel/crypto/hkdf.c — HMAC-SHA256 + HKDF-SHA256, DDR-818.
 *
 * Straight transcription of RFC 2104 and RFC 5869 on top of DDR-811's SHA-256.
 * Deliberately unoptimised: this derives a handful of 32-byte keys per boot, so
 * clarity beats throughput and portability beats both.
 */
#include "hkdf.h"

#define BLOCK 64u                      /* SHA-256 block size, for HMAC padding */

void hmac_sha256(const void *key, uint32_t klen,
                 const void *msg, uint32_t mlen,
                 uint8_t out[SHA256_DIGEST_LEN]) {
    uint8_t k[BLOCK];
    uint8_t pad[BLOCK];
    uint8_t inner[SHA256_DIGEST_LEN];
    sha256_ctx c;

    /* RFC 2104: keys longer than the block are hashed down; shorter ones are
     * zero-padded to the block. Both cases end with exactly BLOCK bytes in k. */
    for (uint32_t i = 0; i < BLOCK; i++)
        k[i] = 0;
    if (klen > BLOCK) {
        sha256(key, klen, k);          /* remaining bytes stay zero */
    } else {
        const uint8_t *kp = (const uint8_t *)key;
        for (uint32_t i = 0; i < klen; i++)
            k[i] = kp[i];
    }

    /* inner = H((k ^ ipad) || msg) */
    for (uint32_t i = 0; i < BLOCK; i++)
        pad[i] = (uint8_t)(k[i] ^ 0x36u);
    sha256_init(&c);
    sha256_update(&c, pad, BLOCK);
    sha256_update(&c, msg, mlen);
    sha256_final(&c, inner);

    /* out = H((k ^ opad) || inner) */
    for (uint32_t i = 0; i < BLOCK; i++)
        pad[i] = (uint8_t)(k[i] ^ 0x5Cu);
    sha256_init(&c);
    sha256_update(&c, pad, BLOCK);
    sha256_update(&c, inner, SHA256_DIGEST_LEN);
    sha256_final(&c, out);
}

int hkdf_sha256(const void *salt, uint32_t saltlen,
                const void *ikm,  uint32_t ikmlen,
                const void *info, uint32_t infolen,
                uint8_t *okm,     uint32_t okmlen) {
    if (!okm || okmlen == 0 || okmlen > HKDF_MAX_OKM)
        return -1;

    /* ---- extract: PRK = HMAC(salt, IKM) ----
     * RFC 5869 §2.2: an absent salt is HashLen ZERO BYTES, not a zero-length
     * string. Getting this wrong still produces plausible output and is
     * invisible unless a vector exercises it — hence vector 3. */
    uint8_t zero_salt[SHA256_DIGEST_LEN];
    if (!salt || saltlen == 0) {
        for (uint32_t i = 0; i < SHA256_DIGEST_LEN; i++)
            zero_salt[i] = 0;
        salt    = zero_salt;
        saltlen = SHA256_DIGEST_LEN;
    }
    uint8_t prk[SHA256_DIGEST_LEN];
    hmac_sha256(salt, saltlen, ikm, ikmlen, prk);

    /* ---- expand: T(n) = HMAC(PRK, T(n-1) || info || n) ----
     * T(0) is empty, so the first iteration hashes info||1 with no prefix. An
     * implementation that only ever computes T(1) satisfies any OKM of 32 bytes
     * or fewer — which is why the gate carries an 82-byte vector. */
    uint8_t t[SHA256_DIGEST_LEN];
    uint32_t tlen = 0;                 /* 0 on the first round: T(0) is empty */
    uint32_t done = 0;
    uint8_t  counter = 1;

    while (done < okmlen) {
        /* HMAC over the concatenation, streamed so no scratch buffer is needed
         * for T(n-1) || info || counter. */
        uint8_t k[BLOCK], pad[BLOCK], inner[SHA256_DIGEST_LEN];
        sha256_ctx c;

        for (uint32_t i = 0; i < BLOCK; i++)
            k[i] = (i < SHA256_DIGEST_LEN) ? prk[i] : 0u;

        for (uint32_t i = 0; i < BLOCK; i++)
            pad[i] = (uint8_t)(k[i] ^ 0x36u);
        sha256_init(&c);
        sha256_update(&c, pad, BLOCK);
        if (tlen)
            sha256_update(&c, t, tlen);
        if (infolen)
            sha256_update(&c, info, infolen);
        sha256_update(&c, &counter, 1);
        sha256_final(&c, inner);

        for (uint32_t i = 0; i < BLOCK; i++)
            pad[i] = (uint8_t)(k[i] ^ 0x5Cu);
        sha256_init(&c);
        sha256_update(&c, pad, BLOCK);
        sha256_update(&c, inner, SHA256_DIGEST_LEN);
        sha256_final(&c, t);
        tlen = SHA256_DIGEST_LEN;

        uint32_t take = okmlen - done;
        if (take > SHA256_DIGEST_LEN)
            take = SHA256_DIGEST_LEN;
        for (uint32_t i = 0; i < take; i++)
            okm[done + i] = t[i];
        done += take;
        counter++;
    }
    return 0;
}
