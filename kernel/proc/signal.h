/* kernel/proc/signal.h — POSIX signal delivery (Phase 5b, PROC-C).
 *
 * Minimal set: SIGKILL (9, terminate), SIGTERM (15, default
 * terminate), SIGUSR1 (10, catchable). A per-TCB pending bitmap + handler table;
 * delivery happens when a ring-3 thread is about to resume from a timer IRQ
 * (idt.c). A caught signal snapshots the interrupted register frame, redirects
 * to the handler, and sys_sigreturn restores the snapshot via IRETQ.
 *
 * DDR-1090 — SIGKILL WAS DESCRIBED HERE AS "UNBLOCKABLE" AND IT WAS NOT.
 * signal_deliver() is called from two places and BOTH are guarded by
 * `(r->cs & 3) == 3`, so a signal is acted on only when returning to RING 3. A
 * thread inside an unbounded kernel wait (both pipe waits and the blocking
 * console read in sys_io.c, poll/epoll_wait with timeout < 0, mnt_lock) is in
 * ring 0 at every timer IRQ, so the bit was recorded and never acted on --
 * measured, `reaped=-11` three wall seconds after the kill. SIGKILL is now also
 * honoured at yield(), the choke point all those waits share. EVERY OTHER
 * SIGNAL WAS RING-3-RETURN ONLY at the time of writing.
 *
 * DDR-1174 adds a blocked mask, per-signal flags (SA_SIGINFO, SA_RESTART),
 * kernel-recorded sender info, SIGCHLD, EINTR from the blocking waits, and
 * delivery on SYSCALL EXIT (syscall_entry.asm), which is what makes EINTR and
 * SA_RESTART correct. Every write to sig_pending goes through sig_raise /
 * atomic clears: the sender and the receiver run on different CPUs.
 */
#pragma once
#include <stdint.h>

struct regs;
struct tcb;

#define NSIG     32
#define SIGKILL  9
#define SIGUSR1  10
/* DDR-805: raised on a ring-3 write to a pipe with no readers left. POSIX
 * numbering throughout this header — an arbitrary value would mislead anyone
 * comparing against `kill -l`. Default action terminates. */
#define SIGPIPE  13
#define SIGTERM  15
#define SIGCHLD  17          /* DDR-1174: default action ignore */

/* sigaction flags (Linux values, so ported code reads naturally). */
#define SA_SIGINFO  0x00000004u
#define SA_RESTART  0x10000000u

/* sigprocmask `how`. */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* si_code values used here. */
#define SI_USER     0
#define SI_KERNEL   0x80
#define CLD_EXITED  1

/* Ring-3 ABI of SYS_SIGACTION2 (NSI 109). Layout pinned in signal.c. */
struct k_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t mask;
};

/* What an SA_SIGINFO handler receives (pinned; user/sigsemtest.c copies it). */
struct k_siginfo {
    int32_t  signo;
    int32_t  code;
    int32_t  pid;
    int32_t  status;
};

/* Kernel-side record of who sent a signal; ring 3 cannot write it. */
struct sig_rec {
    int32_t pid;
    int32_t code;
    int32_t status;
};

#define SIG_UNMASKABLE ((1ull << SIGKILL))

void signal_register(void);                 /* SYS_SIGACTION(2) / _KILL / _SIGRETURN / _SIGPROCMASK */
void signal_deliver(struct regs *r);        /* IRQ return to ring 3, and syscall exit */
/* Set `sig` pending on `t` with sender info. Atomic on sig_pending; the caller
 * must make `t` lifetime-safe (g_sched_lock held, or t == current_thread). */
void sig_raise(struct tcb *t, int sig, int32_t pid, int32_t code, int32_t status);
/* 1 if a pending, unmasked signal with a handler is waiting: a blocking wait
 * must return -EINTR. Ignored and default-action signals do not interrupt
 * (SIGKILL is handled separately at yield(), DDR-1090). */
int  signal_interruptible(const struct tcb *t);
/* DDR-1181 A1.3: SIGKILL is pending (it has no handler, so the above misses it). */
int  signal_kill_pending(const struct tcb *t);
/* Reset caught handlers to default at exec (POSIX); mask and pending survive. */
void signal_exec_reset(struct tcb *t);
/* Copy dispositions and mask at fork (POSIX); pending is NOT inherited. */
void signal_fork_copy(struct tcb *child, const struct tcb *parent);
