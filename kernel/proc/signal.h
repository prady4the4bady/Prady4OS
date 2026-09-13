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
 * SIGNAL IS STILL RING-3-RETURN ONLY: a catchable signal cannot interrupt a
 * syscall here, which is why `EINTR` appears nowhere in this tree.
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
