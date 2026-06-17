/* kernel/ipc/bcast.c — sovereign broadcast bus (Phase 2c).
 *
 * Single-CPU for now: a cli/sti critical section guards the subscriber list and
 * each subscriber's queue, and closes the lost-wakeup race the same way the
 * synchronous endpoint does.
 */
#include "bcast.h"
#include "sched.h"

static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }

void bcast_bus_init(struct bcast_bus *b, uint64_t res_id) {
    b->res_id = res_id;
    b->subs = 0;
}

int bcast_subscribe(struct cap_table *caps, cap_t h, struct bcast_bus *b,
                    struct bcast_subscriber *s, uint32_t mask) {
    if (!cap_authorize(caps, h, RES_IPC, b->res_id, CAP_IPC_RECV))
        return -1;
    s->mask = mask;
    s->head = s->tail = 0;
    s->waiter = 0;
    cli();
    s->next = b->subs;
    b->subs = s;
    sti();
    return 0;
}

static void enqueue(struct bcast_subscriber *s, uint32_t type, uint64_t payload) {
    uint32_t next = (s->tail + 1) % BCAST_QUEUE;
    if (next == s->head)
        return;                          /* queue full: drop (telemetry semantics) */
    s->q[s->tail].type = type;
    s->q[s->tail].payload = payload;
    s->tail = next;
}

int bcast_publish(struct cap_table *caps, cap_t h, struct bcast_bus *b,
                  uint32_t type, uint64_t payload) {
    if (!cap_authorize(caps, h, RES_IPC, b->res_id, CAP_BROADCAST))
        return -1;
    cli();
    for (struct bcast_subscriber *s = b->subs; s; s = s->next) {
        if (s->mask & type) {
            enqueue(s, type, payload);
            if (s->waiter) {
                struct tcb *w = s->waiter;
                s->waiter = 0;
                sched_unblock(w);
            }
        }
    }
    sti();
    return 0;
}

void bcast_wait(struct bcast_subscriber *s, struct bcast_event *out) {
    cli();
    while (s->head == s->tail) {
        s->waiter = current_thread;
        sched_block();
    }
    *out = s->q[s->head];
    s->head = (s->head + 1) % BCAST_QUEUE;
    sti();
}
