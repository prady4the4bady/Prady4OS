/* kernel/proc/signal.h — POSIX signal delivery (Phase 5b, PROC-C).
 *
 * Minimal set: SIGKILL (9, unblockable terminate), SIGTERM (15, default
 * terminate), SIGUSR1 (10, catchable). A per-TCB pending bitmap + handler table;
 * delivery happens when a ring-3 thread is about to resume from a timer IRQ
 * (idt.c). A caught signal snapshots the interrupted register frame, redirects
 * to the handler, and sys_sigreturn restores the snapshot via IRETQ.
 */
#pragma once

struct regs;

#define NSIG     32
#define SIGKILL  9
#define SIGUSR1  10
/* DDR-805: raised on a ring-3 write to a pipe with no readers left. POSIX
 * numbering throughout this header — an arbitrary value would mislead anyone
 * comparing against `kill -l`. Default action terminates. */
#define SIGPIPE  13
#define SIGTERM  15

void signal_register(void);                 /* SYS_SIGACTION / _KILL / _SIGRETURN */
void signal_deliver(struct regs *r);        /* called from the IRQ return path */
