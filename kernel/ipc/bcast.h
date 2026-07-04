/* kernel/ipc/bcast.h — Sovereign Broadcast Bus (kernel pub-sub), Phase 2c.
 *
 * A kernel-resident publish/subscribe bus. Subscribers register an interest mask
 * and drain a per-subscriber event queue (blocking until an event arrives).
 * Publishing is capability-gated (CAP_BROADCAST, bound to the bus). This is the
 * mechanism the SOVEREIGN/MANUAL approval system will use: the compositor
 * subscribes to APPROVAL_REQUEST, aetherd publishes proposals — no polling.
 */
#pragma once
#include <stdint.h>
#include "cap.h"
#include "spinlock.h"

/* System-wide event types (bitmask, so a subscriber can want several). */
#define EVT_AGENT_PROPOSAL   (1u << 0)
#define EVT_RESOURCE_ALERT   (1u << 1)
#define EVT_APPROVAL_REQUEST (1u << 2)
#define EVT_MODE_CHANGE      (1u << 3)

#define BCAST_QUEUE 32

struct tcb;   /* forward decl */

struct bcast_event {
    uint32_t type;
    uint64_t payload;
};

struct bcast_subscriber {
    uint32_t mask;                       /* event types this subscriber wants */
    uint32_t head, tail;                 /* per-subscriber event ring          */
    struct bcast_event q[BCAST_QUEUE];
    struct tcb *waiter;                  /* blocked subscriber to wake         */
    struct bcast_subscriber *next;       /* linked into the bus                */
    spinlock_t lock;                     /* DDR-SMP-3c-locks-4: guards q + waiter
                                          * (MPSC: many publishers, one drainer) */
};

struct bcast_bus {
    uint64_t res_id;
    struct bcast_subscriber *subs;
    spinlock_t lock;                     /* DDR-SMP-3c-locks-4: guards the subs list */
};

void bcast_bus_init(struct bcast_bus *b, uint64_t res_id);
/* Register `s` for `mask`. Needs a bus capability with CAP_IPC_RECV. */
int  bcast_subscribe(struct cap_table *caps, cap_t h, struct bcast_bus *b,
                     struct bcast_subscriber *s, uint32_t mask);
/* Publish to all matching subscribers. Needs a bus capability with CAP_BROADCAST. */
int  bcast_publish(struct cap_table *caps, cap_t h, struct bcast_bus *b,
                   uint32_t type, uint64_t payload);
/* Block until an event is queued for `s`, then dequeue it. */
void bcast_wait(struct bcast_subscriber *s, struct bcast_event *out);
