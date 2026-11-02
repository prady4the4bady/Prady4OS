/* kernel/proc/kmutex.c — sleeping kernel mutex (DDR-1181 §3.3). */
#include "kmutex.h"
#include "sched.h"

void kmutex_lock(struct kmutex *m) {
    struct tcb *self = current_thread;
    uint64_t fl = spin_lock_irqsave(&m->lk);
    if (!m->owner) {
        m->owner = self;
        spin_unlock_irqrestore(&m->lk, fl);
        return;
    }
    self->kmx_next = 0;
    if (m->tail) m->tail->kmx_next = self; else m->head = self;
    m->tail = self;
    /* kmutex_unlock hands ownership over BEFORE it wakes us, so the loop's
     * condition is the hand-off itself; a spurious wake just blocks again. */
    while (m->owner != self)
        sched_block_on(&m->lk);
    spin_unlock_irqrestore(&m->lk, fl);
}

void kmutex_unlock(struct kmutex *m) {
    uint64_t fl = spin_lock_irqsave(&m->lk);
    struct tcb *next = m->head;
    if (next) {
        m->head = next->kmx_next;
        if (!m->head) m->tail = 0;
        next->kmx_next = 0;
    }
    m->owner = next;                 /* hand-off: NULL when nobody waits */
    spin_unlock_irqrestore(&m->lk, fl);
    if (next)
        sched_unblock(next);
}
