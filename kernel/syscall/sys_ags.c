/* kernel/syscall/sys_ags.c — AGS ring-3 interface (DDR-814), NSI 79/80.
 *
 * CAPABILITY SPLIT — the inverse of ACC's, for the same underlying reason:
 * the privileged direction is whichever one an attacker would want.
 *
 *   SYS_GOAL_SIGN   — CAP_SOVEREIGN. Signing AUTHORISES a goal. If an agent
 *                     could sign, it could authorise its own goals and the
 *                     audit log would show a valid signature on something
 *                     nobody approved (S1: no self-escalation).
 *
 *   SYS_GOAL_VERIFY — CAP_AGENT. Verification touches only public data, and an
 *                     agent SHOULD be able to check the goals handed to it.
 *                     Refusing this would push agents toward trusting unsigned
 *                     input, which is the opposite of the point.
 */
#include "syscall.h"
#include "sched.h"
#include "uaccess.h"
#include "errno.h"
#include "cap.h"
#include "aether.h"
#include "ags.h"

#define AGS_MAX_GOAL 512u        /* S2: bounded, never unbounded */

/* ring-3 ABI: (sig_out, goal, goallen, owner_seed) */
static long sys_goal_sign(long a1, long a2, long a3, long a4) {
    if (!current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    uint32_t glen = (uint32_t)a3;
    if (glen == 0 || glen > AGS_MAX_GOAL)
        return -EINVAL;

    uint8_t goal[AGS_MAX_GOAL], seed[32], sig[AGS_SIG_LEN];
    if (copyin(goal, (const void __user *)a2, glen) < 0)
        return -EFAULT;
    if (copyin(seed, (const void __user *)a4, sizeof seed) < 0)
        return -EFAULT;

    if (ags_sign(sig, goal, glen, seed) != AGS_OK)
        return -EINVAL;
    if (copyout((void __user *)a1, sig, sizeof sig) < 0)
        return -EFAULT;

    aether_audit(current_thread->pid, 0, glen, AR_GOAL_SIGNED);
    return 0;
}

/* ring-3 ABI: (sig, goal, goallen, owner_pub) */
static long sys_goal_verify(long a1, long a2, long a3, long a4) {
    if (!current_thread->is_agent && !current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    uint32_t glen = (uint32_t)a3;
    if (glen == 0 || glen > AGS_MAX_GOAL)
        return -EINVAL;

    uint8_t goal[AGS_MAX_GOAL], pub[32], sig[AGS_SIG_LEN];
    if (copyin(sig, (const void __user *)a1, sizeof sig) < 0)
        return -EFAULT;
    if (copyin(goal, (const void __user *)a2, glen) < 0)
        return -EFAULT;
    if (copyin(pub, (const void __user *)a4, sizeof pub) < 0)
        return -EFAULT;

    if (ags_verify(sig, goal, glen, pub) != AGS_OK) {
        /* Recorded distinctly from AR_CAP_DENIED: "this goal is not the
         * owner's" is a forgery attempt, not a policy refusal, and collapsing
         * them would make "was a forged goal ever presented?" unanswerable. */
        aether_audit(current_thread->pid, 0, glen, AR_GOAL_REJECTED);
        return -EACCES;
    }
    return 0;
}

void sys_ags_register(void) {
    syscall_register(SYS_GOAL_SIGN,   sys_goal_sign);    /* NSI 79 */
    syscall_register(SYS_GOAL_VERIFY, sys_goal_verify);  /* NSI 80 */
}
