/* kernel/proc/signal.c — POSIX signal delivery (Phase 5b PROC-C; DDR-1174).
 *
 * sys_sigaction(signum, handler_va) — the original one-pointer ABI, kept for
 * every shipped probe — is SYS_SIGACTION2 with flags=0, mask=0. SYS_SIGACTION2
 * takes {handler, flags, mask}; SYS_SIGPROCMASK edits the blocked set; sys_kill
 * raises through sched_signal_user_pid (lookup + raise under g_sched_lock,
 * DDR-1159); sys_sigreturn restores the frame and the pre-handler mask.
 *
 * Delivery (signal_deliver) runs on the timer-IRQ return to ring 3 (idt.c) AND
 * on syscall exit (syscall_entry.asm's slow path, DDR-1174 §2.3). The handler
 * runs with mask = old | sa_mask | bit(sig). SA_SIGINFO handlers get a
 * kernel-written siginfo on the user stack. SA_RESTART rewinds an -EINTR'd
 * syscall so sigreturn re-executes it.
 */
#include <stddef.h>
#include "signal.h"
#include "sched.h"
#include "regs.h"
#include "syscall.h"
#include "errno.h"
#include "uaccess.h"
#include "aether.h"     /* DDR-1170: time budget on the ring-3 timer return */

extern void signal_sigreturn(struct regs *saved);   /* arch/x86_64/usermode.asm */

/* syscall_entry.asm reads these two words on every syscall exit. Its %defines
 * (TCB_SIG_PENDING / TCB_SIG_MASK) must match; a layout change fails here. */
#define TCB_SIG_PENDING_OFF 5320
#define TCB_SIG_MASK_OFF    5768
_Static_assert(offsetof(struct tcb, sig_pending) == TCB_SIG_PENDING_OFF,
               "update TCB_SIG_PENDING in syscall_entry.asm");
_Static_assert(offsetof(struct tcb, sig_mask) == TCB_SIG_MASK_OFF,
               "update TCB_SIG_MASK in syscall_entry.asm");
_Static_assert(sizeof(struct k_sigaction) == 24, "k_sigaction ABI");
_Static_assert(sizeof(struct k_siginfo) == 16, "k_siginfo ABI");

#define SYSCALL_INSN_LEN 2    /* 0F 05 */

void sig_raise(struct tcb *t, int sig, int32_t pid, int32_t code, int32_t status) {
    if (sig <= 0 || sig >= NSIG)
        return;
    t->sig_info[sig].pid    = pid;
    t->sig_info[sig].code   = code;
    t->sig_info[sig].status = status;
    /* Release: the info above is visible before the bit that announces it. */
    __atomic_fetch_or(&t->sig_pending, 1ull << sig, __ATOMIC_RELEASE);
}

static uint64_t deliverable(const struct tcb *t) {
    return __atomic_load_n(&t->sig_pending, __ATOMIC_ACQUIRE) &
           ~(t->sig_mask & ~SIG_UNMASKABLE);
}

int signal_interruptible(const struct tcb *t) {
    uint64_t d = deliverable(t);
    for (int sig = 1; sig < NSIG; sig++)
        if ((d & (1ull << sig)) && t->sig_handlers[sig])
            return 1;
    return 0;
}

void signal_exec_reset(struct tcb *t) {
    for (int i = 0; i < NSIG; i++) {
        t->sig_handlers[i] = 0;   /* the old image's handler VAs mean nothing now */
        t->sig_flags[i] = 0;
        t->sig_amask[i] = 0;
    }
    t->sig_active = 0;
}

void signal_fork_copy(struct tcb *child, const struct tcb *parent) {
    for (int i = 0; i < NSIG; i++) {
        child->sig_handlers[i] = parent->sig_handlers[i];
        child->sig_flags[i]    = parent->sig_flags[i];
        child->sig_amask[i]    = parent->sig_amask[i];
    }
    child->sig_mask = parent->sig_mask;
}

static long do_sigaction(int sig, const struct k_sigaction *act, struct k_sigaction *old) {
    struct tcb *t = current_thread;
    if (old) {
        old->handler = t->sig_handlers[sig];
        old->flags   = t->sig_flags[sig];
        old->mask    = t->sig_amask[sig];
    }
    if (act) {
        t->sig_handlers[sig] = act->handler;
        t->sig_flags[sig]    = (uint32_t)(act->flags & (SA_SIGINFO | SA_RESTART));
        t->sig_amask[sig]    = act->mask & ~SIG_UNMASKABLE;
    }
    return 0;
}

static long sys_sigaction(long a_signum, long a_handler, long a3, long a4, long a5, long a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    int sig = (int)a_signum;
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL)
        return -EINVAL;                         /* SIGKILL cannot be caught */
    struct k_sigaction k = { (uint64_t)a_handler, 0, 0 };
    return do_sigaction(sig, &k, 0);
}

static long sys_sigaction2(long a_signum, long a_act, long a_old, long a4, long a5, long a6) {
    (void)a4; (void)a5; (void)a6;
    int sig = (int)a_signum;
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL)
        return -EINVAL;
    struct k_sigaction act, old;
    if (a_act && copyin(&act, (const void __user *)a_act, sizeof act))
        return -EFAULT;
    do_sigaction(sig, a_act ? &act : 0, &old);
    if (a_old && copyout((void __user *)a_old, &old, sizeof old))
        return -EFAULT;
    return 0;
}

static long sys_sigprocmask(long a_how, long a_set, long a_old, long a4, long a5, long a6) {
    (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_thread;
    uint64_t old = t->sig_mask, set = 0;
    if (a_set) {
        if (copyin(&set, (const void __user *)a_set, sizeof set))
            return -EFAULT;
        switch (a_how) {
        case SIG_BLOCK:   t->sig_mask |= set;  break;
        case SIG_UNBLOCK: t->sig_mask &= ~set; break;
        case SIG_SETMASK: t->sig_mask  = set;  break;
        default: return -EINVAL;
        }
        t->sig_mask &= ~SIG_UNMASKABLE;
    }
    if (a_old && copyout((void __user *)a_old, &old, sizeof old))
        return -EFAULT;
    return 0;
}

static long sys_kill(long a_pid, long a_signum, long a3, long a4, long a5, long a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    int sig = (int)a_signum;
    if (sig <= 0 || sig >= NSIG)
        return -EINVAL;
    /* DDR-1174 §1.2: was an unlocked ring walk + a plain |=. The locked helper
     * (DDR-1159) does the lookup and the raise under g_sched_lock. */
    return sched_signal_user_pid((uint32_t)a_pid, sig);
}

static long sys_sigreturn(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_thread;
    if (!t->sig_active)
        return -EINVAL;                         /* not in a handler */
    t->sig_active = 0;
    t->sig_mask = t->sig_saved_mask & ~SIG_UNMASKABLE;
    signal_sigreturn(&t->sig_saved);            /* restores the snapshot; never returns */
    return 0;                                   /* unreachable */
}

void signal_deliver(struct regs *r) {
    struct tcb *t = current_thread;
    /* DDR-1170: the timer ISR's ring-3 return paths are the only point a task
     * spinning in ring 3 with no syscalls is ever seen. On syscall exit the
     * budget is charged by the dispatcher already; checking again is harmless. */
    if (t->is_user && aether_budget_time(t) < 0)
        sched_exit(137);                         /* never returns */
    if (!t->is_user || t->sig_active)
        return;
    uint64_t d = deliverable(t);
    if (!d)
        return;
    for (int sig = 1; sig < NSIG; sig++) {
        uint64_t bit = 1ull << sig;
        if (!(d & bit))
            continue;
        __atomic_fetch_and(&t->sig_pending, ~bit, __ATOMIC_ACQ_REL);

        if (sig == SIGKILL)
            sched_exit(-1);                      /* never returns */
        uint64_t h = t->sig_handlers[sig];
        if (!h) {
            /* DDR-805: SIGPIPE joins SIGTERM in the default-terminate set. */
            if (sig == SIGTERM || sig == SIGPIPE)
                sched_exit(-1);
            continue;                            /* ignored / default-ignore (SIGCHLD) */
        }

        /* SA_RESTART: an -EINTR'd syscall frame (orig_rax carried in
         * r->vector by the syscall-exit path, which sets err_code = 1) is
         * rewound so sigreturn re-executes it. */
        if (r->err_code == 1 && (int64_t)r->rax == -EINTR &&
            (t->sig_flags[sig] & SA_RESTART)) {
            r->rax = r->vector;
            r->rip -= SYSCALL_INSN_LEN;
        }

        t->sig_saved = *r;
        t->sig_saved_mask = t->sig_mask;
        t->sig_mask |= (t->sig_amask[sig] | bit) & ~SIG_UNMASKABLE;
        t->sig_active = 1;
        r->rdi = (uint64_t)sig;
        r->rip = h;
        if (t->sig_flags[sig] & SA_SIGINFO) {
            struct k_siginfo si = { sig, t->sig_info[sig].code,
                                    t->sig_info[sig].pid, t->sig_info[sig].status };
            uint64_t sp = (r->rsp - 128 - sizeof si) & ~0xFull;  /* past the red zone */
            if (copyout((void __user *)sp, &si, sizeof si)) {
                sched_exit(-1);                  /* cannot build the frame: SIGSEGV-like */
            }
            r->rsi = sp;
            r->rdx = 0;                          /* no ucontext */
            /* Handler entry must look like a CALL: rsp % 16 == 8. */
            r->rsp = sp - 8;
        }
        return;
    }
}

void signal_register(void) {
    syscall_register(SYS_SIGACTION,   sys_sigaction);
    syscall_register(SYS_SIGACTION2,  sys_sigaction2);
    syscall_register(SYS_SIGPROCMASK, sys_sigprocmask);
    syscall_register(SYS_KILL,        sys_kill);
    syscall_register(SYS_SIGRETURN,   sys_sigreturn);
}
