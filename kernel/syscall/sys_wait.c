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
 * deferred; init logs it raw). Runs with IF clear (SYSCALL entry), so the
 * check-then-block sequence cannot lose a wakeup.
 */
#include "sys_wait.h"
#include "sched.h"
#include "vmm.h"          /* vmm_destroy_address_space */
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
 * The walk now lives in sched.c as `sched_find_child`, where g_sched_lock is
 * reachable, following the pattern `sched_snapshot` already set for this exact
 * traversal. */

static long sys_wait4(long a_pid, long a_status, long a_options, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a4;
    int pid     = (int)a_pid;
    int options = (int)a_options;
    struct tcb *self = current_thread;

    struct tcb *live, *child;
    int any;
    /* Find a reapable zombie; block (or EAGAIN) while children remain but none
     * has exited. IF is clear here, so a child can only run (and exit) once we
     * sched_block — no lost-wakeup window. */
    int ringbad = 0;
    while (!(child = sched_find_child(self, pid, &live, &any, &ringbad))) {
        if (ringbad) {
            /* Should never happen: under g_sched_lock the ring is consistent.
             * If it is not, say so and return rather than spin — a bounded,
             * attributable failure beats a CPU that cannot be interrupted. */
            kputs("[ringwalk] wait4 ring inconsistent pid=");
            kputdec(self->pid);
            kputs("\n");
            return -ECHILD;
        }
        if (!any)
            return -ECHILD;            /* no matching children at all */
        if (options & WNOHANG)
            return -EAGAIN;            /* children exist, none exited yet */
        live->waiter = self;           /* any child's exit wakes us; we re-scan */
        sched_block();
    }

    int status = child->exit_status;
    if (a_status) {
        if (copyout((void __user *)(uintptr_t)a_status, &status, sizeof status) < 0)
            return -EFAULT;            /* bad status ptr: leave the zombie unreaped */
    }

    int child_pid  = (int)child->pid;
    uint64_t child_cr3 = child->cr3;
    sched_destroy(child);              /* unlink + free kstack/caps/fds/TCB */
    if (child_cr3)
        vmm_destroy_address_space(child_cr3);
    return child_pid;
}

void sys_wait_register(void) {
    syscall_register(SYS_WAIT4, sys_wait4);
}
