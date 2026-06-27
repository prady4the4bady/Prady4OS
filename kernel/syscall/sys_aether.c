/* kernel/syscall/sys_aether.c — AETHER NSI handlers (ADR-026 / DDR §1, §4).
 *
 * The 10 agent-layer syscalls (29..38). Every user pointer crosses copyin/copyout
 * (ADR-022); authority is the kernel-set per-process flag (is_agent / is_sovereign),
 * never a user-forgeable token. A denied authority returns -EPERM and audits the
 * attempt but lets the caller survive (only resource abuse is fatal).
 */
#include "syscall.h"
#include "sched.h"
#include "uaccess.h"
#include "errno.h"
#include "console.h"
#include "aether.h"

#define SIGKILL 9

/* Walk the ready ring for a live user thread with this pid. */
static struct tcb *tcb_by_pid(uint32_t pid) {
    struct tcb *t = current_thread;
    do {
        if (t->is_user && t->pid == pid &&
            t->state != THREAD_ZOMBIE && t->state != THREAD_DONE)
            return t;
        t = t->next;
    } while (t != current_thread);
    return 0;
}

/* The kernel-side agent spawner is provided by kmain (it closes over the SFS
 * mount + cap + embedded agent ELF). Returns the new pid, or <0. */
typedef long (*aether_spawn_fn)(const char *task);
static aether_spawn_fn g_spawn_hook;
void aether_set_spawn_hook(aether_spawn_fn fn) { g_spawn_hook = fn; }

static long sys_get_mode(long a1, long a2, long a3, long a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    return (long)aether_get_mode();
}

static long sys_set_mode(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    if (!current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;                          /* no self-escalation (D6) */
    }
    return aether_set_mode((unsigned)a1);
}

static long sys_submit_action(long a1, long a2, long a3, long a4) {
    (void)a4;
    if (!current_thread->is_agent) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    uint32_t type = (uint32_t)a1;
    uint32_t len  = (uint32_t)a3;
    if (len > AETHER_PAYLOAD_MAX) len = AETHER_PAYLOAD_MAX;
    uint8_t kbuf[AETHER_PAYLOAD_MAX];
    if (len && copyin(kbuf, (const void __user *)a2, len) < 0)
        return -EFAULT;
    return aether_submit(current_thread->pid, type, kbuf, len);
}

static long sys_poll_result(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    return aether_poll(current_thread->pid, (uint64_t)a1);
}

static long sys_approve_action(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    if (!current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    return aether_approve((uint64_t)a1, 1);
}

static long sys_reject_action(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    if (!current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    return aether_approve((uint64_t)a1, 0);
}

static long sys_spawn_agent(long a1, long a2, long a3, long a4) {
    (void)a1; (void)a3; (void)a4;
    /* The daemon (sovereign) or an existing agent may spawn agents (D6/D8). */
    if (!current_thread->is_sovereign && !current_thread->is_agent) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    if (!g_spawn_hook)
        return -ENOSYS;
    char task[64];
    if (a2 && copyinstr(task, (const void __user *)a2, sizeof task, 0) < 0)
        return -EFAULT;
    if (!a2) task[0] = 0;
    return g_spawn_hook(task);
}

static long sys_kill_agent(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    if (!current_thread->is_sovereign && !current_thread->is_agent)
        return -EPERM;
    struct tcb *t = tcb_by_pid((uint32_t)a1);
    if (!t)
        return -ESRCH;
    t->sig_pending |= (1ull << SIGKILL);        /* terminated on its next IRQ return */
    return 0;
}

static long sys_read_audit(long a1, long a2, long a3, long a4) {
    (void)a3; (void)a4;
    int max = (int)a2;
    if (max <= 0) return 0;
    if (max > 64) max = 64;                      /* bounded kernel staging buffer */
    struct aether_audit_entry_pub buf[64];
    int n = aether_audit_read(buf, max);
    if (n > 0 && copyout((void __user *)a1, buf,
                         (size_t)n * sizeof buf[0]) < 0)
        return -EFAULT;
    return n;
}

static long sys_set_mem_limit(long a1, long a2, long a3, long a4) {
    (void)a3; (void)a4;
    long r = aether_set_mem_limit((uint32_t)a1, (uint64_t)a2);
    return r < 0 ? -EPERM : 0;                   /* lower-only; no self-escalation */
}

void sys_aether_register(void) {
    syscall_register(SYS_GET_MODE,       sys_get_mode);
    syscall_register(SYS_SET_MODE,       sys_set_mode);
    syscall_register(SYS_SUBMIT_ACTION,  sys_submit_action);
    syscall_register(SYS_POLL_RESULT,    sys_poll_result);
    syscall_register(SYS_APPROVE_ACTION, sys_approve_action);
    syscall_register(SYS_REJECT_ACTION,  sys_reject_action);
    syscall_register(SYS_SPAWN_AGENT,    sys_spawn_agent);
    syscall_register(SYS_KILL_AGENT,     sys_kill_agent);
    syscall_register(SYS_READ_AUDIT,     sys_read_audit);
    syscall_register(SYS_SET_MEM_LIMIT,  sys_set_mem_limit);
}
