/* kernel/syscall/sys_experiment.c — DDR-1034: the ring-3 door to the bounded
 * experiment executor, and the read side of its results store.
 *
 * TWO LAYERS, and DDR-1033's lesson is why they are tested apart:
 *   is_exec  — may this process use the door at all. Kernel-set at spawn by
 *              exec_grant(); there is no path from ring 3 that sets it.
 *   CAP_EXEC — a RES_EXEC handle, checked by cap_authorize.
 * Two checks in series each mask the other's absence, so the gate's deny
 * process HOLDS the capability and lacks only the flag (DDR-1034 §2/§7 arm B).
 *
 * The results store is read-only from here. Its only writer is exp_run(), in
 * the kernel: no syscall records a result, so an agent cannot report a value it
 * did not compute. That reproduces the DDR-812 lockbox property WITHOUT
 * touching, extending or reading the lockbox.
 */
#include "syscall.h"
#include "sched.h"
#include "uaccess.h"
#include "cap.h"
#include "experiment.h"
#include "errno.h"
#include <stdint.h>

/* One res_id for the subsystem: the capability grants "may run experiments",
 * not "may run this experiment". Coarse, and stated as such (DDR-1034 §5). */
#define EXEC_RES_ID 0x45585045ull            /* "EXPE" */

/* Grant the door. Kernel-only, like ipc_grant -- never reachable from ring 3,
 * which is what keeps this out of self-escalation territory. */
void exec_grant(struct tcb *t)
{
    if (!t || !t->caps)
        return;
    t->is_exec  = 1;
    t->exec_cap = cap_create(t->caps, RES_EXEC, EXEC_RES_ID, CAP_EXEC);
}

static long sys_run_experiment(long a1, long a2, long a3, long a4, long a5, long a6)
{
    (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_thread;

    if (!t || !t->is_exec)
        return -EPERM;                        /* the door itself */
    if (!cap_authorize(t->caps, t->exec_cap, RES_EXEC, EXEC_RES_ID, CAP_EXEC))
        return -EPERM;                        /* the capability */

    if (a2 <= 0 || (uint32_t)a2 > EXP_MAX_CODE)
        return -EINVAL;

    /* Copy the program in BEFORE running it. The executor reads kernel memory
     * only, so a ring-3 unmap mid-run cannot fault the interpreter. */
    uint8_t code[EXP_MAX_CODE];
    if (copyin(code, (const void __user *)(uintptr_t)a1, (unsigned long)a2) < 0)
        return -EFAULT;

    int64_t value = 0;
    int rc = exp_run(code, (uint32_t)a2, (uint32_t)t->pid, &value);
    if (rc != 0)
        return rc;                            /* recorded either way */
    if (a3 && copyout((void __user *)(uintptr_t)a3, &value, sizeof value) < 0)
        return -EFAULT;
    return 0;
}

static long sys_exp_result(long a1, long a2, long a3, long a4, long a5, long a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_thread;

    /* Read is NOT privileged and the store is NOT secret: the guarantee is
     * write integrity, not read privacy (DDR-1034 §5, limit 1). */
    if (!t || (!t->is_exec && !t->is_sovereign))
        return -EPERM;
    if (a1 < 0)
        return -EINVAL;

    exp_result_t r;
    int rc = exp_result_get((uint32_t)a1, &r);
    if (rc != 0)
        return rc;
    if (copyout((void __user *)(uintptr_t)a2, &r, sizeof r) < 0)
        return -EFAULT;
    return 0;
}

void sys_experiment_register(void)
{
    syscall_register(SYS_RUN_EXPERIMENT, sys_run_experiment);  /* DDR-1034 */
    syscall_register(SYS_EXP_RESULT,     sys_exp_result);      /* DDR-1034 */
}
