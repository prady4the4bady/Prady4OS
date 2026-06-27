/* kernel/aether/aether_mem.c — AETHER per-process memory cap + syscall rate limit.
 *
 * Both enforce a hard bound on an agent and, when crossed, signal the *caller* to
 * cleanly kill the offending process (zombie + status, reaped by its parent) —
 * never a kernel panic. A kernel-thread/init/PRISM process (is_agent==0) is
 * exempt so existing gates are unaffected (ADR-026 D5/D7).
 *
 * The kill DECISION (log + audit + return <0) is separated from the kill ACTION
 * (sched_exit, done by the caller) so the policy is unit-testable in-kernel
 * without terminating the running thread (see aether_sectest).
 */
#include "aether.h"
#include "sched.h"
#include "console.h"
#include "irq.h"      /* g_ticks */

static void log_pid(const char *tag, uint32_t pid) {
    kputs(tag);
    kputdec(pid);
    kputs("\r\n");
}

static uint64_t cap_of(const struct tcb *t) {
    return t->mem_limit ? t->mem_limit : AETHER_MEM_DEFAULT;
}

/* Returns 0 (allowed, charged) or -1 (over cap: logged + audited; caller kills). */
int aether_mem_charge(struct tcb *t, uint64_t bytes) {
    if (!t->is_agent)
        return 0;                               /* non-agents are uncapped */
    if (t->mem_used + bytes > cap_of(t)) {
        aether_drop_pid(t->pid);
        aether_audit(t->pid, 0, 0, AR_OOM_KILL);
        log_pid("AGENT_OOM_KILLED PID=", t->pid);
        return -1;
    }
    t->mem_used += bytes;
    return 0;
}

void aether_mem_uncharge(struct tcb *t, uint64_t bytes) {
    if (!t->is_agent)
        return;
    t->mem_used = (t->mem_used > bytes) ? t->mem_used - bytes : 0;
}

/* SYS_SET_MEM_LIMIT: a process may only *lower* its own cap (no self-escalation).
 * Raising, or a zero limit, is rejected (-EPERM at the syscall boundary). */
long aether_set_mem_limit(uint32_t pid, uint64_t bytes) {
    (void)pid;
    struct tcb *t = current_thread;
    uint64_t cur = cap_of(t);
    if (bytes == 0 || bytes > cur)
        return -1;
    t->mem_limit = bytes;
    return 0;
}

/* Sliding 1 s window, AETHER_RATE_MAX syscalls. Returns 0 (proceed) or -1 (budget
 * blown: logged + audited; caller kills). */
int aether_rate_check(struct tcb *t) {
    if (!t->is_agent)
        return 0;
    if (g_ticks - t->sc_window_start >= AETHER_RATE_WINDOW) {
        t->sc_window_start = g_ticks;
        t->sc_count = 0;
    }
    if (++t->sc_count > AETHER_RATE_MAX) {
        aether_drop_pid(t->pid);
        aether_audit(t->pid, 0, 0, AR_RATE_KILL);
        log_pid("AGENT_RATE_LIMITED PID=", t->pid);
        return -1;
    }
    return 0;
}
