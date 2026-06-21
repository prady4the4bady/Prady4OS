/* kernel/syscall/sys_wait.c — sys_wait4: reap a child process (Phase 5b slice 9).
 *
 * Completes the process lifecycle: sched_exit now leaves an exited thread in the
 * THREAD_ZOMBIE state holding its exit status (see sched.c) until a parent
 * collects it here (or the reaper sweeps an orphan). sys_wait4 finds the caller's
 * child by pid, blocks until it is a zombie (unless WNOHANG), copies the status
 * out, and reclaims the child's address space + TCB.
 *
 * Baseline scope: a specific positive pid (pid==-1 "any child" is deferred); the
 * status is the raw exit code (POSIX W* encoding is deferred). Runs with IF clear
 * (SYSCALL entry), so the check-then-block sequence cannot lose a wakeup.
 */
#include "sys_wait.h"
#include "sched.h"
#include "vmm.h"          /* vmm_destroy_address_space */
#include "uaccess.h"      /* copyout */
#include "syscall.h"      /* syscall_register, SYS_WAIT4 */
#include "errno.h"
#include <stddef.h>

#define WNOHANG 1

/* The caller's child with process id `pid`, or NULL if none (scans the ring). */
static struct tcb *find_child(struct tcb *parent, int pid) {
    struct tcb *t = parent;
    do {
        if (t != parent && (int)t->pid == pid && t->parent_pid == parent->pid)
            return t;
        t = t->next;
    } while (t != parent);
    return NULL;
}

static long sys_wait4(long a_pid, long a_status, long a_options, long a4) {
    (void)a4;
    int pid     = (int)a_pid;
    int options = (int)a_options;
    struct tcb *self = current_thread;

    struct tcb *child = find_child(self, pid);
    if (!child)
        return -ECHILD;

    /* Wait for the child to become a zombie. IF is clear here, so the child can
     * only run (and exit) once we sched_block — no lost-wakeup window. */
    while (child->state != THREAD_ZOMBIE) {
        if (options & WNOHANG)
            return -EAGAIN;
        child->waiter = self;
        sched_block();                 /* woken by the child's sched_exit */
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
