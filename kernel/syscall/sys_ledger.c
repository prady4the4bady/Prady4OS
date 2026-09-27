/* kernel/syscall/sys_ledger.c -- SYS_LEDGER (NSI 106), DDR-1150.
 *
 * DDR-1059 Route 3: the audit chain's head, signed with a PER-INSTALL ML-DSA-44
 * key whose public half is published off the machine at install time. What this
 * adds over the bare SHA-256 chain is an ANCHOR: an editor can recompute the
 * chain end to end, but cannot produce a signature the published pk accepts
 * unless it also holds this install's secret key. It does NOT defend against a
 * reader of the key file -- DDR-1150 sec.2 states that boundary and it goes into
 * the release notes verbatim.
 *
 * SOVEREIGN-ONLY, every op. An agent must never sign, or re-key, the ledger that
 * records it.
 *
 * The key is generated from rng_bytes(), which FAILS CLOSED (DDR-816). There is
 * deliberately NO fallback to g_owner_seed or to any constant: a key every copy
 * of the image shares is DDR-1059 sec.2's theatre, and a fallback would turn an
 * absent entropy source into exactly that, silently.
 *
 * The secret key never leaves the kernel. The one exception is stated: KEYGEN
 * copies the 32-byte SEED out to the sovereign caller that created it, because
 * the installer must write it to the target disk (DDR-1150 sec.3.3). No other op
 * returns it, and LOAD accepts a seed back at boot.
 *
 * Signing runs with IF clear (SYSCALL masks it) and costs tens of ms under TCG;
 * the rejection loop is bounded (DDR-1057). The number is measured in the gate,
 * and this path is not exonerated in advance (DDR-1042). */
#include "syscall.h"
#include "sched.h"
#include "uaccess.h"
#include "errno.h"
#include "string.h"
#include "pmm.h"
#include "spinlock.h"
#include "aether.h"
#include "rng.h"
#include "mldsa.h"
#include "ledger.h"
#include "crypto/sha256.h"

_Static_assert(LEDGER_SEED_BYTES == MLDSA44_SEED_BYTES, "ledger.h seed size");
_Static_assert(LEDGER_PK_BYTES == MLDSA44_PK_BYTES, "ledger.h pk size");

#define LEDGER_KEYGEN  1
#define LEDGER_LOAD    2
#define LEDGER_PUBKEY  3
#define LEDGER_SIGN    4

#define LEDGER_DOMAIN     "PRADYOS-LEDGER-v1"          /* 17 chars + NUL = 18 */
#define LEDGER_MSG_BYTES  (18u + 8u + 32u)             /* domain | written | head */

_Static_assert(sizeof(LEDGER_DOMAIN) == 18, "ledger message format moved");
_Static_assert(LEDGER_MSG_BYTES == 58, "ledger message format moved");

static spinlock_t g_ledger_lock;
static int        g_have_key;
static uint8_t    g_seed[MLDSA44_SEED_BYTES];
static uint8_t    g_pk[MLDSA44_PK_BYTES];
static uint8_t    g_sk[MLDSA44_SK_BYTES];

/* Scratch from the PMM, on first use: most boots never sign, so ~86 KiB of
 * .bss would be a cost every boot paid for a feature it did not use. */
static void *g_kg_scratch;
static void *g_sig_scratch;

static unsigned order_for(unsigned long bytes) {
    unsigned o = 0;
    while ((4096ul << o) < bytes) o++;
    return o;
}

static void *scratch_get(void **slot, unsigned long bytes) {
    if (!*slot) {
        uint64_t pa = pmm_alloc_pages(order_for(bytes));
        if (pa) {
            /* ZEROED, and it is load-bearing: the scratch carries its own
             * tables_ready flag, and the PMM does not zero. A garbage non-zero
             * flag skips building the NTT tables, keygen emits a wrong key and
             * signing exhausts its 1,000-iteration bound with IF masked --
             * measured on the first run of smoke-ledger (DDR-1150 sec.7). */
            memset((void *)(uintptr_t)pa, 0, 4096ul << order_for(bytes));
            *slot = (void *)(uintptr_t)pa;          /* low identity map */
        }
    }
    return *slot;
}

/* Called with g_ledger_lock held. */
static int install_seed(const uint8_t seed[MLDSA44_SEED_BYTES]) {
    mldsa44_scratch *s = scratch_get(&g_kg_scratch, sizeof(mldsa44_scratch));
    if (!s) return -ENOMEM;
    memcpy(g_seed, seed, MLDSA44_SEED_BYTES);
    mldsa44_keygen(g_seed, g_pk, g_sk, s);
    g_have_key = 1;
    return 0;
}

/* The ONE seed source (DDR-1153 sec.5): KEYGEN and the installer share it. */
int ledger_new_seed(uint8_t seed[LEDGER_SEED_BYTES]) {
    return rng_bytes(seed, LEDGER_SEED_BYTES) < 0 ? -EIO : 0;   /* fails closed */
}

/* Derive a key for the INSTALL TARGET. The held key (g_have_key/g_pk/g_sk) is
 * not touched: installing must not re-key the running system. */
int ledger_derive_pk(const uint8_t seed[LEDGER_SEED_BYTES], uint8_t pk[LEDGER_PK_BYTES]) {
    static uint8_t sk[MLDSA44_SK_BYTES];
    spin_lock(&g_ledger_lock);
    mldsa44_scratch *s = scratch_get(&g_kg_scratch, sizeof(mldsa44_scratch));
    int r = -ENOMEM;
    if (s) {
        uint8_t sd[MLDSA44_SEED_BYTES];
        memcpy(sd, seed, sizeof sd);
        mldsa44_keygen(sd, pk, sk, s);
        memset(sd, 0, sizeof sd);
        memset(sk, 0, sizeof sk);
        r = 0;
    }
    spin_unlock(&g_ledger_lock);
    return r;
}

/* Load the installed seed at boot (DDR-1153 sec.2). The key is kept ONLY if
 * sha256(keygen(seed).pk) equals the digest the installer stored beside it; a
 * seed that no longer produces its recorded key -- a torn or edited sector --
 * would sign under a key nobody published, so it is refused and forgotten. */
int ledger_load_seed_verified(const uint8_t seed[LEDGER_SEED_BYTES],
                              const uint8_t fp[32]) {
    uint8_t got[32];
    spin_lock(&g_ledger_lock);
    int r = g_have_key ? -EEXIST : install_seed(seed);
    if (r == 0) {
        sha256(g_pk, MLDSA44_PK_BYTES, got);
        if (memcmp(got, fp, 32) != 0) {
            g_have_key = 0;
            memset(g_seed, 0, sizeof g_seed);
            memset(g_sk, 0, sizeof g_sk);
            memset(g_pk, 0, sizeof g_pk);
            r = -ETAMPER;           /* stored digest does not match */
        }
    }
    spin_unlock(&g_ledger_lock);
    return r;
}

static long ledger_keygen(long ubuf, long ulen) {
    if ((unsigned long)ulen < MLDSA44_SEED_BYTES) return -EINVAL;
    uint8_t seed[MLDSA44_SEED_BYTES];
    if (ledger_new_seed(seed) < 0) return -EIO;         /* fails closed */
    spin_lock(&g_ledger_lock);
    int r = g_have_key ? -EEXIST : install_seed(seed);
    spin_unlock(&g_ledger_lock);
    if (r == 0 && copyout((void __user *)ubuf, seed, sizeof seed) < 0)
        r = -EFAULT;           /* key is held; the caller merely lost its copy */
    memset(seed, 0, sizeof seed);
    return r;
}

static long ledger_load(long ubuf, long ulen) {
    if ((unsigned long)ulen != MLDSA44_SEED_BYTES) return -EINVAL;
    uint8_t seed[MLDSA44_SEED_BYTES];
    if (copyin(seed, (const void __user *)ubuf, sizeof seed) < 0) return -EFAULT;
    spin_lock(&g_ledger_lock);
    int r = g_have_key ? -EEXIST : install_seed(seed);
    spin_unlock(&g_ledger_lock);
    memset(seed, 0, sizeof seed);
    return r;
}

static long ledger_pubkey(long ubuf, long ulen) {
    if ((unsigned long)ulen < MLDSA44_PK_BYTES) return -EINVAL;
    if (!g_have_key) return -ENOKEY;
    if (copyout((void __user *)ubuf, g_pk, MLDSA44_PK_BYTES) < 0) return -EFAULT;
    return MLDSA44_PK_BYTES;
}

/* Out: message (58 B) then signature (2420 B). Returns the message length. */
static long ledger_sign(long ubuf, long ulen) {
    if ((unsigned long)ulen < LEDGER_MSG_BYTES + MLDSA44_SIG_BYTES) return -EINVAL;
    if (!g_have_key) return -ENOKEY;

    /* Refuse to put this install's signature on a chain already known to be
     * tampered with: that would certify the tampering. */
    uint32_t bad = 0;
    if (aether_audit_verify(&bad) != 0) return -ETAMPER;

    static uint8_t out[LEDGER_MSG_BYTES + MLDSA44_SIG_BYTES];
    uint64_t written = 0;
    uint8_t  head[32];
    aether_audit_head(&written, head);

    spin_lock(&g_ledger_lock);
    uint8_t *m = out;
    memcpy(m, LEDGER_DOMAIN, 18);
    for (int i = 0; i < 8; i++) m[18 + i] = (uint8_t)(written >> (8 * i));
    memcpy(m + 26, head, 32);
    mldsa44_sign_scratch *s = scratch_get(&g_sig_scratch, sizeof(mldsa44_sign_scratch));
    int r = s ? mldsa44_sign_internal(g_sk, m, LEDGER_MSG_BYTES,
                                      out + LEDGER_MSG_BYTES, s)
              : -ENOMEM;
    if (r == 0 && copyout((void __user *)ubuf, out, sizeof out) < 0)
        r = -EFAULT;
    spin_unlock(&g_ledger_lock);
    if (r == -1) return -EIO;                   /* rejection bound exceeded */
    return r < 0 ? r : (long)LEDGER_MSG_BYTES;
}

static long sys_ledger(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a4; (void)a5; (void)a6;
    if (!current_thread->is_sovereign) return -EPERM;   /* every op, first */
    switch (a1) {
    case LEDGER_KEYGEN: return ledger_keygen(a2, a3);
    case LEDGER_LOAD:   return ledger_load(a2, a3);
    case LEDGER_PUBKEY: return ledger_pubkey(a2, a3);
    case LEDGER_SIGN:   return ledger_sign(a2, a3);
    default:            return -EINVAL;
    }
}

void sys_ledger_register(void) {
    syscall_register(SYS_LEDGER, sys_ledger);            /* NSI 106 */
}
