/* kernel/proc/kmutex.c — sleeping kernel mutex (DDR-1181 §3.3). */
#include "kmutex.h"
#include "sched.h"

void kmutex_lock(struct kmutex *m) {
    struct tcb *self = current_thread;
    uint64_t fl = spin_lock_irqsave(&m->lk);
    if (!m->owner) {
        m->owner = self;
        self->kmx_held++;
        spin_unlock_irqrestore(&m->lk, fl);
        return;
    }
    self->kmx_next = 0;
    if (m->tail) m->tail->kmx_next = self; else m->head = self;
    m->tail = self;
    /* DDR-1188 A4 S6: the owner runs at no less than this waiter's priority
     * until it releases its last kmutex. */
    uint32_t me = sched_eff_prio(self);
    if (me > sched_eff_prio(m->owner))
        sched_pi_raise(m->owner, me);
    /* kmutex_unlock hands ownership over BEFORE it wakes us, so the loop's
     * condition is the hand-off itself; a spurious wake just blocks again. */
    while (m->owner != self)
        sched_block_on(&m->lk);
    self->kmx_held++;
    spin_unlock_irqrestore(&m->lk, fl);
}

void kmutex_unlock(struct kmutex *m) {
    struct tcb *self = current_thread;
    uint64_t fl = spin_lock_irqsave(&m->lk);
    struct tcb *next = m->head;
    if (next) {
        m->head = next->kmx_next;
        if (!m->head) m->tail = 0;
        next->kmx_next = 0;
        /* DDR-1188 A4 S6: the new owner inherits from the waiters still queued. */
        uint32_t top = 0;
        for (struct tcb *w = m->head; w; w = w->kmx_next) {
            uint32_t p = sched_eff_prio(w);
            if (p > top) top = p;
        }
        if (top > sched_eff_prio(next))
            sched_pi_raise(next, top);
    }
    m->owner = next;                 /* hand-off: NULL when nobody waits */
    if (self->kmx_held)
        self->kmx_held--;
    int drop = (self->kmx_held == 0 && self->pi_prio);
    spin_unlock_irqrestore(&m->lk, fl);
    if (drop)
        sched_pi_clear(self);        /* S6: last kmutex released, boost ends */
    if (next)
        sched_unblock(next);
}
