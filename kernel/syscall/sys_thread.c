/* kernel/syscall/sys_thread.c — SYS_THREAD_CREATE / SYS_THREAD_EXIT (DDR-1181 P1c).
 *
 * thread_create(entry, stack_top, arg, tls, clear_tid) -> tid | -errno
 *   A new thread in the caller's address space (struct mm, shared) and
 *   capability table (shared); fds and signal dispositions are copied
 *   (DDR-1181 §3.6). It starts in ring 3 at `entry` with RSP = stack_top,
 *   RDI = arg and FS base = tls. When it exits, the kernel writes a u32 0 at
 *   clear_tid (if nonzero).
 * thread_exit(status)
 *   Ends the calling thread only. SYS_EXIT still ends the whole process.
 */
#include "sched.h"
#include "mm.h"
#include "vmm.h"
#include "syscall.h"
#include "errno.h"

#define THREAD_MAX_PER_MM  64        /* bound on live threads in one process */

static long sys_thread_create(long a_entry, long a_stack, long a_arg,
                              long a_tls, long a_ctid, long a6) {
    (void)a6;
    struct tcb *t = current_thread;
    uint64_t entry = (uint64_t)a_entry, stack = (uint64_t)a_stack;
    uint64_t tls = (uint64_t)a_tls, ctid = (uint64_t)a_ctid;

    /* DDR-1181 §3.2: refused where a thread would escape an accounting
     * boundary that is still per-tcb: a rollback task's log (as fork is,
     * DDR-1171), an agent's and a budgeted task's limits (DDR-1170). */
    if (!t->mm || t->rb_log || t->is_agent ||
        t->bud_max_ticks || t->bud_max_bytes || t->bud_max_actions)
        return -EPERM;

    /* The same bound SYS_SET_TLS applies: a kernel or non-canonical FS base
     * would #GP the WRMSR at the first switch-in. */
    if (tls != 0 && (tls < VMM_USER_MIN || tls >= VMM_USER_MAX))
        return -EINVAL;
    if ((stack & 15) || stack < 16 ||
        !vmm_user_range_ok(t->cr3, stack - 16, 16, 1))
        return -EINVAL;
    if (!vmm_user_range_ok(t->cr3, entry, 1, 0))
        return -EINVAL;
    if (ctid && ((ctid & 3) || !vmm_user_range_ok(t->cr3, ctid, 4, 1)))
        return -EINVAL;
    if (__atomic_load_n(&t->mm->live, __ATOMIC_ACQUIRE) >= THREAD_MAX_PER_MM)
        return -EAGAIN;

    uint32_t tid = sched_create_user_thread(t, entry, stack, (uint64_t)a_arg, tls, ctid);
    if (!tid)
        return t->mm->group_set ? -EAGAIN : -ENOMEM;  /* group exiting, or no memory */
    return (long)tid;
}

static long sys_thread_exit(long a_status, long a2, long a3, long a4, long a5, long a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    sched_exit_thread((int)a_status);           /* never returns */
    return 0;
}

void sys_thread_register(void) {
    syscall_register(SYS_THREAD_CREATE, sys_thread_create);
    syscall_register(SYS_THREAD_EXIT,   sys_thread_exit);
}
