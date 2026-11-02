/* kernel/syscall/sys_wait.c — sys_wait4: reap a child process (Phase 5b slice 9).
 *
 * Completes the process lifecycle: sched_exit now leaves an exited thread in the
 * THREAD_ZOMBIE state holding its exit status (see sched.c) until a parent
 * collects it here (or the reaper sweeps an orphan). sys_wait4 finds the caller's
 * child by pid, blocks until it is a zombie (unless WNOHANG), copies the status
 * out, and reclaims the child's address space + TCB.
 *
 * Scope: a specific positive pid, OR pid==-1 ("any child" — 5d, needed by PID 1
 * init's reap loop). The status is the raw exit code (POSIX W* encoding is
 * deferred; init logs it raw). The check-then-block sequence runs under
 * g_sched_lock in sched_wait_child, so it cannot lose a wakeup on SMP either
 * (DDR-1181 §4.6).
 */
#include "sys_wait.h"
#include "sched.h"
#include "mm.h"           /* DDR-1181: mm_put destroys the address space */
#include "uaccess.h"      /* copyout */
#include "syscall.h"      /* syscall_register, SYS_WAIT4 */
#include "errno.h"
#include "console.h"   /* DDR-1001 sentinel */
#include <stddef.h>

#define WNOHANG 1

/* DDR-1001: the local, UNLOCKED `find_zombie_child` that used to live here is
 * gone. It walked the all-threads ring taking no lock while sched_ring_unlink
 * relinked it under g_sched_lock — a lock only excludes participants who take
 * it — so the walk could follow a stale `->next` and never terminate, with IF
 * clear from SYSCALL entry so nothing could preempt it. Measured as
 * `[apfreeze] … bt=…,sys_wait4+0x4f,…` on smoke-smpuser (DDR-1001).
 *
 * The walk now lives in sched.c as `sched_wait_child`, where g_sched_lock is
 * reachable, following the pattern `sched_snapshot` already set for this exact
 * traversal. */

static long sys_wait4(long a_pid, long a_status, long a_options, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a4;
    int pid     = (int)a_pid;
    int options = (int)a_options;
    struct tcb *self = current_thread;

    /* DDR-1181 §4.6: the search, the claim and the sleep are one locked loop
     * in sched.c. The child returned is claimed: no other waiter and not the
     * reaper can take it, so the destroy below is the only one. */
    int err = 0;
    struct tcb *child = sched_wait_child(self, pid, options & WNOHANG, &err);
    if (!child)
        return err;                    /* -ECHILD, or -EAGAIN under WNOHANG */

    /* DDR-1181 §3.4: the process status, which a sibling thread's SYS_EXIT or
     * fatal fault may have fixed; for a single-threaded child it is the same
     * value sched_exit stored in exit_status. */
    int status = (child->mm && __atomic_load_n(&child->mm->group_set, __ATOMIC_ACQUIRE))
               ? child->mm->group_status : child->exit_status;
    if (a_status) {
        if (copyout((void __user *)(uintptr_t)a_status, &status, sizeof status) < 0) {
            sched_unclaim_child(child);    /* bad status ptr: leave it unreaped */
            return -EFAULT;
        }
    }

    int child_pid  = (int)child->pid;
    struct mm *child_mm = child->mm;   /* DDR-1181: owns the child's page tables */
    sched_destroy(child);              /* unlink + free kstack/caps/fds/TCB */
    mm_put(child_mm);                  /* last ref destroys the address space */
    return child_pid;
}

void sys_wait_register(void) {
    syscall_register(SYS_WAIT4, sys_wait4);
}
