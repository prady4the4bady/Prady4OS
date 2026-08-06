/* kernel/syscall/sys_rewrite.c — code-rewrite approval (DDR-842, NSI 86).
 *
 * S4 makes the human gate structural for code rewrites; S8 requires
 * CAP_SOVEREIGN on any skill.md or code write. The spec is explicit that
 * CAP_REWRITE "always requires CAP_SOVEREIGN co-approval, never auto-granted".
 *
 * CO-APPROVAL MEANS BOTH BITS, CHECKED TOGETHER. A holder of CAP_REWRITE alone
 * is refused, and — the arm that actually matters — a holder of CAP_SOVEREIGN
 * alone is ALSO refused. If sovereignty alone sufficed, CAP_REWRITE would be
 * decoration and every sovereign process would silently be a code-rewrite
 * authority.
 */
#include "syscall.h"
#include "sched.h"
#include "errno.h"
#include "cap.h"
#include "aether.h"

/* ring-3 ABI: (action_id) */
static long sys_approve_code_rewrite(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;

    /* Both bits. Neither alone. */
    if (!current_thread->is_sovereign || !current_thread->is_rewrite) {
        aether_audit(current_thread->pid, ACTION_REWRITE_AGENT_CODE, 0,
                     AR_CAP_DENIED);
        return -EPERM;
    }

    uint64_t action_id = (uint64_t)a1;
    if (action_id == 0)
        return -EINVAL;

    /* Only a code-rewrite action may be approved here. A call that could approve
     * an arbitrary action id would be a general-purpose approval bypass wearing
     * a specific name. */
    uint32_t type = 0;
    if (aether_action_type_of(action_id, &type) != 0)
        return -ESRCH;
    if (type != ACTION_REWRITE_AGENT_CODE)
        return -EINVAL;

    long r = aether_approve(action_id, 1);
    if (r != 0)
        return r;

    aether_audit(current_thread->pid, ACTION_REWRITE_AGENT_CODE, action_id,
                 AR_CODE_REWRITE_APPROVED);
    return 0;
}

void sys_rewrite_register(void) {
    syscall_register(SYS_APPROVE_CODE_REWRITE, sys_approve_code_rewrite); /* 86 */
}
