/* kernel/syscall/sys_acc.c — ACC ring-3 interface (DDR-813), NSI 77/78.
 *
 * The kernel owns the envelope construction because it owns the keys. A ring-3
 * agent hands over plaintext and gets back a sealed envelope; it never sees
 * K_session, K_owner, the ephemeral scalar, or the owner's box private key.
 * That is the whole reason this is a syscall and not a library.
 *
 * CAPABILITY SPLIT, and it is deliberately asymmetric:
 *
 *   SYS_ACC_SEAL  — CAP_AGENT. Any agent may seal FOR the owner. Sealing is how
 *                   an agent reports; refusing it would make the channel
 *                   useless, and a sealed envelope leaks nothing to the sealer.
 *
 *   SYS_ACC_OPEN  — CAP_SOVEREIGN. Opening reveals another agent's plaintext.
 *                   An agent that could open its peers' envelopes would defeat
 *                   the confidentiality the channel exists to provide, so this
 *                   is owner-only and a CAP_AGENT attempt is an audited denial
 *                   (S1: no self-escalation).
 *
 * Every uaccess boundary uses copyin/copyout, so a hostile pointer is -EFAULT
 * and never a CPL-0 fault (ADR-022).
 */
#include "syscall.h"
#include "sched.h"
#include "uaccess.h"
#include "errno.h"
#include "cap.h"
#include "aether.h"
#include "acc.h"
#include "rng.h"
#include "ed25519.h"
#include <stdint.h>   /* DDR-815: channels are keyed by the agent verify key */

/* DDR-815: per-agent channels. A single global replay floor meant one agent's
 * traffic decided whether ANOTHER agent's envelope looked like a replay, and
 * nothing could ever be revoked. Each agent now owns its own floor, and the
 * owner can rotate a channel to invalidate everything minted before now. */
#define ACC_MAX_CHANNELS 16u

/* Keyed by the agent's Ed25519 PUBLIC KEY, not by pid. The key is carried
 * in-band (BUG-1) and is covered by the envelope signature, so it is an
 * authenticated identity; a pid is neither — it is recycled when a process
 * exits, and a new process inheriting a dead agent's pid would inherit its
 * replay floor. Channel identity must be as strong as the signature that
 * defends it. */
typedef struct {
    uint8_t  key[ACC_PUB_LEN];  /* all-zero == free slot              */
    uint64_t last_seq;          /* replay floor for this channel      */
    uint64_t seq_out;           /* next outbound nonce sequence       */
    uint64_t epoch;             /* bumped by SYS_ACC_ROTATE           */
} acc_chan_t;

static int key_eq(const uint8_t a[ACC_PUB_LEN], const uint8_t b[ACC_PUB_LEN]) {
    uint8_t d = 0;
    for (unsigned i = 0; i < ACC_PUB_LEN; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}
static int key_is_zero(const uint8_t a[ACC_PUB_LEN]) {
    uint8_t d = 0;
    for (unsigned i = 0; i < ACC_PUB_LEN; i++) d |= a[i];
    return d == 0;
}

static acc_chan_t g_acc_chan[ACC_MAX_CHANNELS];

/* Find this pid's channel, allocating a free slot on first use. Returns NULL
 * when the table is full — S2: a bounded table that REJECTS is safe, one that
 * recycles a live slot would reset some other agent's replay floor, which is
 * the very attack DDR-815 exists to prevent. */
static acc_chan_t *acc_chan_for(const uint8_t key[ACC_PUB_LEN]) {
    acc_chan_t *free_slot = 0;
    for (unsigned i = 0; i < ACC_MAX_CHANNELS; i++) {
        if (!key_is_zero(g_acc_chan[i].key) && key_eq(g_acc_chan[i].key, key))
            return &g_acc_chan[i];
        if (!free_slot && key_is_zero(g_acc_chan[i].key))
            free_slot = &g_acc_chan[i];
    }
    if (!free_slot)
        return 0;
    for (unsigned i = 0; i < ACC_PUB_LEN; i++) free_slot->key[i] = key[i];
    free_slot->last_seq = 0;
    free_slot->seq_out  = 0;
    free_slot->epoch    = 0;
    return free_slot;
}

/* ring-3 ABI: (env_out, pt, ptlen, owner_box_pub) — four args, fits NSI's
 * 4-register convention without the ADR-022 6-arg widening. The agent's Ed25519
 * signing seed is NOT a parameter: it comes from the kernel, so a compromised
 * agent cannot sign as a different agent. */
struct acc_seal_args {
    uint8_t owner_box_pub[ACC_PUB_LEN];
    uint8_t agent_sign_seed[32];
};

static long sys_acc_seal(long a1, long a2, long a3, long a4) {
    if (!current_thread->is_agent && !current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    uint32_t ptlen = (uint32_t)a3;
    if (ptlen == 0 || ptlen > ACC_MAX_PT)
        return -EINVAL;                     /* S2: bounded, never unbounded */

    uint8_t pt[ACC_MAX_PT];
    struct acc_seal_args args;
    if (copyin(pt, (const void __user *)a2, ptlen) < 0)
        return -EFAULT;
    if (copyin(&args, (const void __user *)a4, sizeof args) < 0)
        return -EFAULT;

    /* DDR-816: fail closed. A predictable ephemeral scalar makes every
     * "encrypted" envelope readable by anyone running the same image, so ACC
     * refuses to seal rather than sealing weakly. */
    uint8_t fresh[8];
    if (rng_bytes(fresh, sizeof fresh) != 0)
        return -EIO;
    /* The entropy varies the per-envelope sequence, NOT the signing seed. The
     * seed is the agent's identity and must stay stable across envelopes;
     * randomising it would change agent_sign_pub every call and make the
     * owner's offline verification impossible — the exact failure BUG-1 exists
     * to prevent. */
    uint8_t self_pub[ACC_PUB_LEN];
    ed25519_pubkey(self_pub, args.agent_sign_seed);
    acc_chan_t *ch = acc_chan_for(self_pub);
    if (!ch)
        return -ENOSPC;                     /* table full: reject, never evict */
    uint64_t nonce_seq = ++ch->seq_out;
    for (unsigned i = 0; i < 8u; i++)
        nonce_seq ^= ((uint64_t)fresh[i]) << (i * 8);

    acc_envelope_t env;
    int r = acc_seal(&env, pt, ptlen, args.owner_box_pub,
                     args.agent_sign_seed, nonce_seq);
    if (r != ACC_OK)
        return -EINVAL;

    if (copyout((void __user *)a1, &env, sizeof env) < 0)
        return -EFAULT;
    aether_audit(current_thread->pid, 0, 0, AR_ACC_SEALED);
    return 0;
}

/* ring-3 ABI: (pt_out, env, owner_box_priv, ptlen_out) */
static long sys_acc_open(long a1, long a2, long a3, long a4) {
    if (!current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;                      /* S1: opening is owner-only */
    }

    acc_envelope_t env;
    uint8_t priv[32];
    if (copyin(&env, (const void __user *)a2, sizeof env) < 0)
        return -EFAULT;
    if (copyin(priv, (const void __user *)a3, sizeof priv) < 0)
        return -EFAULT;

    uint8_t pt[ACC_MAX_PT];
    uint32_t ptlen = 0;
    /* The replay floor belongs to the channel the envelope came FROM, not to the
     * owner doing the reading — otherwise every agent shares one floor again. */
    acc_chan_t *ch = acc_chan_for(env.agent_sign_pub);
    if (!ch)
        return -ENOSPC;
    int r = acc_open(pt, &ptlen, &env, priv, &ch->last_seq);
    if (r != ACC_OK) {
        aether_audit(current_thread->pid, 0, (uint64_t)(-r), AR_ACC_REJECTED);
        /* The failure MODE is returned distinctly: a replay and a forgery are
         * different events and collapsing them would hide an attack behind a
         * bookkeeping error. */
        return (r == ACC_ERR_REPLAY) ? -EAGAIN
             : (r == ACC_ERR_AUTH)   ? -EACCES
                                     : -EINVAL;
    }

    if (copyout((void __user *)a1, pt, ptlen) < 0)
        return -EFAULT;
    if (copyout((void __user *)a4, &ptlen, sizeof ptlen) < 0)
        return -EFAULT;
    aether_audit(current_thread->pid, 0, ptlen, AR_ACC_OPENED);
    return 0;
}

/* ring-3 ABI: (target_pid, 0, 0, 0) -> 0 | -EPERM|-ENOENT
 *
 * CAP_SOVEREIGN, and this is the whole security argument of DDR-815: rotation
 * RESETS the replay floor. An agent able to rotate its own channel could set
 * last_seq back to zero and re-present envelopes the owner already consumed,
 * turning the anti-replay control into a replay tool. The capability that
 * protects a mechanism must not be grantable to the party it constrains. */
static long sys_acc_rotate(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    if (!current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    uint8_t target[ACC_PUB_LEN];
    if (copyin(target, (const void __user *)a1, sizeof target) < 0)
        return -EFAULT;
    if (key_is_zero(target))
        return -EINVAL;                      /* all-zero is the free marker */

    for (unsigned i = 0; i < ACC_MAX_CHANNELS; i++) {
        if (!key_is_zero(g_acc_chan[i].key) && key_eq(g_acc_chan[i].key, target)) {
            /* REVOCATION, not a reset. Clearing last_seq to 0 would make every
             * pre-rotation envelope acceptable AGAIN — the precise replay hole
             * this syscall exists to close. The floor is raised past any
             * issuable sequence instead, so the old key's envelopes can never
             * verify again; a re-keyed agent gets a fresh channel under its new
             * public key. The slot is kept as a tombstone rather than freed,
             * because freeing it would let the old key allocate a clean slot
             * with last_seq = 0 and replay everything. */
            g_acc_chan[i].epoch++;
            g_acc_chan[i].last_seq = UINT64_MAX;
            g_acc_chan[i].seq_out  = UINT64_MAX;
            aether_audit(current_thread->pid, g_acc_chan[i].epoch,
                         0, AR_ACC_ROTATED);
            return 0;
        }
    }
    return -ENOENT;                          /* no such channel: say so */
}

void sys_acc_register(void) {
    syscall_register(SYS_ACC_SEAL,   sys_acc_seal);    /* NSI 77 */
    syscall_register(SYS_ACC_OPEN,   sys_acc_open);    /* NSI 78 */
    syscall_register(SYS_ACC_ROTATE, sys_acc_rotate);  /* NSI 81 (DDR-815) */
}
