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
 * sched_cpu(op, cpu)   (DDR-1186, C1.4 P1d)
 *   0 GET: the CPU index the caller runs on. 1 PIN: the calling thread runs
 *   on `cpu` only (a present CPU), from the yield this call ends with. 2 UNPIN.
 *   Self only; not inherited by fork or thread create.
 */
#include "sched.h"
#include "mm.h"
#include "vmm.h"
#include "syscall.h"
#include "errno.h"
#include "kmutex.h"

/* DDR-1188 A4 arm P: set by the launcher when QEMU_PROBES has schedclass;
 * SYS_SCHED_CPU ops 3 and 4 answer -ENOSYS otherwise. */
volatile uint32_t g_sched_probe;
static struct kmutex g_pi_test_mx;      /* zeroed = unlocked */
#include "percpu.h"   /* DDR-1186: cpu_idx, present */

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

static long sys_sched_cpu(long a_op, long a_cpu, long a3, long a4, long a5, long a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct percpu *pc = this_cpu();
    switch (a_op) {
    case 0:                                     /* GET */
        return pc ? (long)pc->cpu_idx : 0;
    case 1: {                                   /* PIN */
        if (a_cpu < 0 || a_cpu >= PERCPU_MAX)
            return -EINVAL;
        struct percpu *tp = percpu_get((uint32_t)a_cpu);
        if (!tp || !tp->present)
            return -EINVAL;
        if (current_thread->sched_policy == SCHED_DEADLINE)
            return -EBUSY;                      /* DDR-1188 A5 D2: its share is per CPU */
        current_thread->pin_cpu = (int32_t)a_cpu;
        /* schedule() re-queues us through rq_push, which honours the pin, so
         * this returns on the pin CPU. */
        yield();
        return 0;
    }
    case 2:                                     /* UNPIN */
        if (current_thread->sched_policy == SCHED_DEADLINE)
            return -EBUSY;                      /* DDR-1188 A5 D2 */
        current_thread->pin_cpu = -1;
        return 0;
    case 3: {                                   /* DDR-1188 A4 arm P: hold the test kmutex */
        if (!g_sched_probe)
            return -ENOSYS;
        if (a_cpu <= 0 || a_cpu > 1000)
            return -EINVAL;
        kmutex_lock(&g_pi_test_mx);
        uint64_t start = current_thread->run_ticks;
        /* Syscalls run with IF clear (SFMASK). Open short windows, the
         * za_hold idiom (DDR-1201), so the tick charges and preempts us while
         * we hold the lock: run_ticks counts only our own CPU time. */
        while (__atomic_load_n(&current_thread->run_ticks, __ATOMIC_RELAXED) - start <
               (uint64_t)a_cpu)
            __asm__ volatile("sti; nop; pause; cli" ::: "memory");
        kmutex_unlock(&g_pi_test_mx);
        return 0;
    }
    case 4:                                     /* DDR-1188 A4 arm P: lock + unlock it */
        if (!g_sched_probe)
            return -ENOSYS;
        kmutex_lock(&g_pi_test_mx);
        kmutex_unlock(&g_pi_test_mx);
        return 0;
    }
    return -EINVAL;
}

/* DDR-1188 Amendment 4 S3: the calling thread's own scheduling policy. */
static long sys_sched_setsched(long a_tid, long a_pol, long a_prio, long a_rt, long a_per, long a6) {
    (void)a6;
    /* Range-checked before the casts below (CodeRabbit on #140). */
    if (a_tid != 0 || a_pol < 0 || a_pol > (long)SCHED_DEADLINE || a_prio < 0 ||
        a_prio > (long)SCHED_RT_MAX)
        return -EINVAL;
    /* DDR-1188 A5 D5: runtime and period (ns) are read only for SCHED_DEADLINE;
     * a FIFO/RR/OTHER caller may leave those registers unset. */
    if (a_pol != (long)SCHED_DEADLINE)
        a_rt = a_per = 0;
    else if (a_rt < 0 || a_per < 0)
        return -EINVAL;
    return sched_setsched_self((uint32_t)a_pol, (uint32_t)a_prio, (uint64_t)a_rt,
                               (uint64_t)a_per);
}

/* DDR-1188 Amendment 4 S3: grant CAP_SCHED_RT to process `pid`. The caller
 * needs CAP_SCHED_GRANT; the right granted cannot include it (no grant-on). */
static long sys_sched_grant(long a_pid, long a_rights, long a3, long a4, long a5, long a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct tcb *self = current_thread;
    if (!cap_authorize(self->caps, self->sched_cap, RES_SCHED, SCHED_RES_ID, CAP_SCHED_GRANT))
        return -EPERM;
    if (a_rights != (long)CAP_SCHED_RT || a_pid <= 0)
        return -EINVAL;
    return sched_grant_pid((uint32_t)a_pid, CAP_SCHED_RT);
}

void sys_thread_register(void) {
    syscall_register(SYS_THREAD_CREATE, sys_thread_create);
    syscall_register(SYS_THREAD_EXIT,   sys_thread_exit);
    syscall_register(SYS_SCHED_CPU,     sys_sched_cpu);
    syscall_register(SYS_SCHED_SETSCHED, sys_sched_setsched);   /* DDR-1188 A4 */
    syscall_register(SYS_SCHED_GRANT,    sys_sched_grant);
}
