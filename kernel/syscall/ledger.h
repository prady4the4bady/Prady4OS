/* kernel/syscall/ledger.h -- kernel-internal ledger key API (DDR-1150, DDR-1153).
 *
 * The installer and root selection use these, so the ledger seed is generated,
 * persisted and reloaded without ever crossing into ring 3 (DDR-1153 sec.2).
 *
 * ledger_new_seed() is the ONE seed source: SYS_LEDGER's KEYGEN and the
 * installer both call it, so a mutant that makes the seed a constant is caught
 * by smoke-ledger's per-keygen arm for both paths by construction rather than
 * by assertion (DDR-1153 sec.5). */
#ifndef PRADYOS_LEDGER_H
#define PRADYOS_LEDGER_H
#include <stdint.h>

#define LEDGER_SEED_BYTES 32u
#define LEDGER_PK_BYTES   1312u

/* rng_bytes, fails closed: 0, or -EIO with no entropy source. No fallback. */
int ledger_new_seed(uint8_t seed[LEDGER_SEED_BYTES]);

/* pk = keygen(seed), without touching the running system's held key.
 * 0 or -ENOMEM. */
int ledger_derive_pk(const uint8_t seed[LEDGER_SEED_BYTES], uint8_t pk[LEDGER_PK_BYTES]);

/* Make seed the running system's key, but only if sha256(keygen(seed).pk)
 * equals fp: 0, -EEXIST, -ENOMEM, or -EBADMSG (refused, nothing held). */
int ledger_load_seed_verified(const uint8_t seed[LEDGER_SEED_BYTES], const uint8_t fp[32]);

#endif
