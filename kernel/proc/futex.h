/* kernel/proc/futex.h — futex wait/wake (DDR-1181 Amendment 1, C1.4 P2). */
#pragma once
#include <stdint.h>

struct tcb;

/* sig_raise: wake `t` if it sleeps in FUTEX_WAIT, so the signal is seen (A1.3). */
void futex_kick(struct tcb *t);

/* Wake up to `n` waiters on the word at user VA `uaddr` of `t`'s address space,
 * keyed as a FUTEX_WAKE without the private flag. Returns the count, or -errno.
 * Used for CLEARTID at thread exit. */
long futex_wake_addr(struct tcb *t, uint64_t uaddr, long n);

/* Register SYS_FUTEX. */
void futex_register(void);
