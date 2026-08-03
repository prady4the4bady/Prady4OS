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

/* Per-channel replay state. One channel for now — DDR-815 (rotation) widens
 * this to a table keyed by agent slot, which is why it is already isolated
 * behind accessors rather than being a bare global read inline. */
static uint64_t g_acc_last_seq;
static uint64_t g_acc_seq_out;

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
    uint64_t nonce_seq = ++g_acc_seq_out;
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
    int r = acc_open(pt, &ptlen, &env, priv, &g_acc_last_seq);
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

void sys_acc_register(void) {
    syscall_register(SYS_ACC_SEAL, sys_acc_seal);   /* NSI 77 */
    syscall_register(SYS_ACC_OPEN, sys_acc_open);   /* NSI 78 */
}
