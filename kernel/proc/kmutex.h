/* kernel/proc/kmutex.h — a sleeping kernel mutex (DDR-1181 §3.3).
 * ===========================================================================
 * The mm lock is held across tlb_shoot, which waits for every target CPU to
 * acknowledge an IPI. A waiter that spun with interrupts off would never
 * acknowledge, and the holder would wait forever. So waiters SLEEP: they queue
 * on the mutex and block (sched_block_on), and the CPU they leave goes on
 * taking IPIs. Ownership is handed to the first waiter on unlock (FIFO), so a
 * waiter is never overtaken by a later locker.
 *
 * Callable only from thread context with no spinlock held (it may block):
 * syscalls and ring-3 fault handling. No priority inheritance (C1.4 P3).
 * ===========================================================================
 */
#pragma once
#include "spinlock.h"

struct tcb;

struct kmutex {
    spinlock_t  lk;          /* guards the three fields below */
    struct tcb *owner;       /* NULL = free */
    struct tcb *head, *tail; /* FIFO of blocked waiters (tcb->kmx_next) */
};

/* A zeroed struct kmutex is a valid unlocked mutex. */
void kmutex_lock(struct kmutex *m);
void kmutex_unlock(struct kmutex *m);
