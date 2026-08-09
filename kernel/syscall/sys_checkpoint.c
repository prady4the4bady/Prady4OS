/* kernel/syscall/sys_checkpoint.c — agent checkpoint/resume (DDR-837, NSI 84/85).
 *
 * Both CAP_SOVEREIGN. Freezing an agent is a denial of service against it, and
 * resuming one undoes a freeze the operator may have applied deliberately —
 * neither belongs to CAP_AGENT. An agent able to resume itself makes the freeze
 * advisory; one able to freeze its peers can silence the agents that would
 * report on it.
 *
 * The target blocks ITSELF at its next syscall boundary (see syscall_dispatch).
 * Setting THREAD_BLOCKED from this CPU would race the scheduler for a thread
 * that may be RUNNING on another CPU mid-kernel-work. See DDR-837.
 */
#include "syscall.h"
#include "sched.h"
#include "errno.h"
#include "cap.h"
#include "aether.h"

/* ring-3 ABI: (pid) */
static long sys_checkpoint_agent(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a2; (void)a3; (void)a4;
    if (!current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    uint32_t pid = (uint32_t)a1;
    if (pid == 0 || pid == 1)
        return -EINVAL;                  /* freezing init wedges the machine */
    if (pid == current_thread->pid)
        return -EINVAL;                  /* would block inside this very call */

    struct tcb *t = sched_find_pid(pid);
    if (!t)
        return -ESRCH;                   /* distinct from -EPERM on purpose */

    t->checkpointed = 1;
    aether_audit(current_thread->pid, pid, 0, AR_AGENT_CHECKPOINT);
    return 0;
}

/* ring-3 ABI: (pid) */
static long sys_resume_agent(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a2; (void)a3; (void)a4;
    if (!current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    uint32_t pid = (uint32_t)a1;
    if (pid == 0)
        return -EINVAL;

    struct tcb *t = sched_find_pid(pid);
    if (!t)
        return -ESRCH;

    /* Clear the flag BEFORE unblocking. The other order lets the target wake,
     * reach its next syscall, still see checkpointed=1 and block again — a
     * resume that silently does nothing. */
    t->checkpointed = 0;
    if (t->state == THREAD_BLOCKED)
        sched_unblock(t);

    aether_audit(current_thread->pid, pid, 0, AR_AGENT_RESUME);
    return 0;
}

void sys_checkpoint_register(void) {
    syscall_register(SYS_CHECKPOINT_AGENT, sys_checkpoint_agent);  /* NSI 84 */
    syscall_register(SYS_RESUME_AGENT,     sys_resume_agent);      /* NSI 85 */
}
