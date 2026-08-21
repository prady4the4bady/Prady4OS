/* kernel/ipc/bcast.c — sovereign broadcast bus (Phase 2c).
 *
 * DDR-SMP-3c-locks-4: two lock grains close the cross-CPU races. The bus lock
 * guards the subscriber LIST (subscribe prepends, publish walks). Each
 * subscriber's own lock guards its queue + waiter — the queue is MPSC (many
 * publishers on different CPUs, one drainer), so per-subscriber exclusion is
 * required. Lock order is always bus -> subscriber; bcast_wait takes only the
 * subscriber lock, so no cycle. The drainer publishes THREAD_BLOCKED under its
 * subscriber lock (sched_block_on) before releasing, closing the lost-wakeup
 * race the same way the synchronous endpoint does.
 */
#include "bcast.h"
#include "errno.h"     /* DDR-955: ETIMEDOUT */
#include "sched.h"

void bcast_bus_init(struct bcast_bus *b, uint64_t res_id) {
    b->res_id = res_id;
    b->subs = 0;
    b->lock = (spinlock_t)SPINLOCK_INIT;
}

int bcast_subscribe(struct cap_table *caps, cap_t h, struct bcast_bus *b,
                    struct bcast_subscriber *s, uint32_t mask) {
    if (!cap_authorize(caps, h, RES_IPC, b->res_id, CAP_IPC_RECV))
        return -1;
    s->mask = mask;
    s->head = s->tail = 0;
    s->waiter = 0;
    s->pending = 0;
    s->lock = (spinlock_t)SPINLOCK_INIT;
    uint64_t flags = spin_lock_irqsave(&b->lock);
    s->next = b->subs;
    b->subs = s;
    spin_unlock_irqrestore(&b->lock, flags);
    return 0;
}

static void enqueue(struct bcast_subscriber *s, uint32_t type, uint64_t payload) {
    uint32_t next = (s->tail + 1) % BCAST_QUEUE;
    if (next == s->head)
        return;                          /* queue full: drop (telemetry semantics) */
    s->q[s->tail].type = type;
    s->q[s->tail].payload = payload;
    s->tail = next;
    s->pending = 1;
}

int bcast_publish(struct cap_table *caps, cap_t h, struct bcast_bus *b,
                  uint32_t type, uint64_t payload) {
    if (!cap_authorize(caps, h, RES_IPC, b->res_id, CAP_BROADCAST))
        return -1;
    uint64_t bflags = spin_lock_irqsave(&b->lock);
    for (struct bcast_subscriber *s = b->subs; s; s = s->next) {
        if (s->mask & type) {
            uint64_t sflags = spin_lock_irqsave(&s->lock);   /* order: bus -> sub */
            enqueue(s, type, payload);
            if (s->waiter) {
                struct tcb *w = s->waiter;
                s->waiter = 0;
                sched_unblock(w);
            }
            spin_unlock_irqrestore(&s->lock, sflags);
        }
    }
    spin_unlock_irqrestore(&b->lock, bflags);
    return 0;
}

/* DDR-961: returns 0 with *out filled, or -ETIMEDOUT with *out untouched.
 * The void signature was the reason DDR-955 could not bound this: an early
 * return handed the caller an unfilled buffer it had no way to detect. */
int bcast_wait(struct bcast_subscriber *s, struct bcast_event *out) {
    uint64_t flags = spin_lock_irqsave(&s->lock);
    while (s->head == s->tail) {
        s->waiter = current_thread;
        /* 500 ticks, matching the virtio-blk sites DDR-955 shipped green rather
         * than the 100 it measured red. */
        if (sched_block_timeout(&s->lock, &s->pending, 500) == -ETIMEDOUT) {
            /* Clear our own registration before leaving -- bcast_publish clears
             * it only on the wake path, so a timed-out waiter that returns and
             * exits would leave s->waiter dangling at a freed TCB for the next
             * publish to sched_unblock() through. That use-after-free is the
             * mechanism behind the lwIP #GPs DDR-955 attributed to a "corrupt
             * bcast buffer"; see DDR-961 sec.2. The == current_thread guard
             * keeps a publisher that won the race from having its clear undone. */
            if (s->waiter == current_thread)
                s->waiter = 0;
            spin_unlock_irqrestore(&s->lock, flags);
            return -ETIMEDOUT;
        }
    }
    *out = s->q[s->head];
    s->head = (s->head + 1) % BCAST_QUEUE;
    if (s->head == s->tail) s->pending = 0;
    spin_unlock_irqrestore(&s->lock, flags);
    return 0;
}
