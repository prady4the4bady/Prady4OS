/* kernel/syscall/sys_fork.c — sys_fork: duplicate the calling process (5b slice 8).
 *
 * Copy-all-pages fork (ADR-022 baseline). The child is an exact copy of the
 * parent's user address space, capability table, and fd table; it resumes at the
 * instruction after the fork SYSCALL with RAX == 0 (delivered by the child's
 * enter_user_mode launch — see §1 CONFIRMED ARCHITECTURAL FACT / sched.c), while
 * the parent returns the child's pid through the normal SYSRET path.
 *
 * The child's resume point is the caller's user RIP/RSP captured at SYSCALL entry
 * (syscall_user_rip / syscall_user_rsp). Registers other than RAX are not
 * replicated in the child (enter_user_mode zeroes them) — a documented baseline
 * limitation; full register-state fork arrives with the signal/trap-frame work.
 */
#include "sys_fork.h"
#include "sched.h"
#include "vmm_fork.h"
#include "vmm.h"
#include "syscall.h"      /* syscall_register, syscall_user_rip/rsp */
#include "errno.h"

static long sys_fork(long a1, long a2, long a3, long a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;        /* fork takes no user args */
    struct tcb *parent = current_thread;

    uint64_t child_cr3 = vmm_fork_address_space_copy(parent->cr3);
    if (!child_cr3)
        return -ENOMEM;

    struct tcb *child = sched_create_user_clone(parent, child_cr3,
                                                syscall_user_rip, syscall_user_rsp);
    if (!child) {
        vmm_destroy_address_space(child_cr3);
        return -ENOMEM;                            /* clone/cap/fd dup failed */
    }
    return (long)child->pid;                       /* parent: child pid; child: 0 via launch */
}

void sys_fork_register(void) {
    syscall_register(SYS_FORK, sys_fork);
}
