/* kernel/proc/signal.c — POSIX signal delivery (Phase 5b, PROC-C).
 *
 * Simplified ABI: sys_sigaction(signum, handler_va) installs a ring-3 handler
 * (one function pointer per signal — no sigaction struct/flags yet); sys_kill
 * (pid, signum) sets the target's pending bit; sys_sigreturn() restores the
 * frame saved at delivery. SIGKILL is non-maskable (always terminates); SIGTERM
 * with no handler terminates; an uncaught catchable signal is ignored.
 *
 * Delivery (signal_deliver, from idt.c's timer-IRQ return to ring 3): snapshot
 * the interrupted *r into the TCB, point RIP at the handler with RDI=signum, and
 * mark sig_active so the handler isn't itself preempted into another delivery.
 * The handler ends by calling sys_sigreturn, which IRETQs back to the snapshot
 * via signal_sigreturn (usermode.asm) — a full register-state restore.
 */
#include "signal.h"
#include "sched.h"
#include "regs.h"
#include "syscall.h"
#include "errno.h"

extern void signal_sigreturn(struct regs *saved);   /* arch/x86_64/usermode.asm */

static long sys_sigaction(long a_signum, long a_handler, long a3, long a4) {
    (void)a3; (void)a4;
    int sig = (int)a_signum;
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL)
        return -EINVAL;                         /* SIGKILL cannot be caught */
    current_thread->sig_handlers[sig] = (uint64_t)a_handler;
    return 0;
}

static long sys_kill(long a_pid, long a_signum, long a3, long a4) {
    (void)a3; (void)a4;
    int pid = (int)a_pid, sig = (int)a_signum;
    if (sig <= 0 || sig >= NSIG)
        return -EINVAL;
    struct tcb *target = 0;
    struct tcb *t = current_thread;
    do {
        if (t->is_user && (int)t->pid == pid) { target = t; break; }
        t = t->next;
    } while (t != current_thread);
    if (!target)
        return -ESRCH;
    target->sig_pending |= (1ull << sig);
    return 0;
}

static long sys_sigreturn(long a1, long a2, long a3, long a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    struct tcb *t = current_thread;
    if (!t->sig_active)
        return -EINVAL;                         /* not in a handler */
    t->sig_active = 0;
    signal_sigreturn(&t->sig_saved);            /* restores the snapshot; never returns */
    return 0;                                   /* unreachable */
}

void signal_deliver(struct regs *r) {
    struct tcb *t = current_thread;
    if (!t->is_user || t->sig_active || !t->sig_pending)
        return;
    for (int sig = 1; sig < NSIG; sig++) {
        if (!(t->sig_pending & (1ull << sig)))
            continue;
        t->sig_pending &= ~(1ull << sig);

        if (sig == SIGKILL) {
            sched_exit(-1);                      /* never returns */
        }
        uint64_t h = t->sig_handlers[sig];
        if (!h) {
            if (sig == SIGTERM)
                sched_exit(-1);                  /* default action: terminate */
            continue;                            /* catchable + no handler: ignore */
        }
        /* Deliver: snapshot the interrupted frame, jump to the handler. */
        t->sig_saved = *r;
        t->sig_active = 1;
        r->rdi = (uint64_t)sig;                  /* handler(int signum) */
        r->rip = h;
        return;
    }
}

void signal_register(void) {
    syscall_register(SYS_SIGACTION, sys_sigaction);
    syscall_register(SYS_KILL,      sys_kill);
    syscall_register(SYS_SIGRETURN, sys_sigreturn);
}
