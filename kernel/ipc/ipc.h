/* kernel/ipc/ipc.h — NEXUS IPC (NIA): synchronous message passing (Phase 2c).
 *
 * A one-slot synchronous endpoint: a receiver blocks until a sender delivers a
 * small fixed message. Every operation is capability-gated — the caller must
 * hold a capability that refers to *this* endpoint (RES_IPC, res_id) and carries
 * the right (CAP_IPC_SEND / CAP_IPC_RECV). Async ring buffers and the sovereign
 * broadcast bus are later slices.
 */
#pragma once
#include <stdint.h>
#include "cap.h"
#include "spinlock.h"

#define IPC_MSG_WORDS 4

struct tcb;   /* forward decl (defined in proc/sched.h) */

struct ipc_endpoint {
    int         full;                  /* 1 if msg holds an undelivered message */
    uint64_t    res_id;                /* capability resource id of this endpoint */
    uint64_t    msg[IPC_MSG_WORDS];
    struct tcb *waiting_receiver;      /* a receiver blocked here, or NULL */
    spinlock_t  lock;                  /* DDR-SMP-3c-locks-4: guards full/msg/waiter */
};

void ipc_endpoint_init(struct ipc_endpoint *e, uint64_t res_id);

/* 0 on success, -1 if the capability does not authorize the operation. */
int  ipc_send(struct cap_table *caps, cap_t h, struct ipc_endpoint *e, const uint64_t *msg);
/* DDR-961: 0 = message received into *out; -1 = capability denied (UNCHANGED,
 * existing callers rely on it); -ETIMEDOUT = deadline expired, *out
 * untouched. A timeout is a value this interface never returned before, so
 * the two failure modes can no longer be confused. */
int  ipc_recv(struct cap_table *caps, cap_t h, struct ipc_endpoint *e, uint64_t *out);

/* --- Async lock-free SPSC ring (one producer, one consumer) ----------------
 * Non-blocking, capability-gated channel for high-frequency telemetry. head and
 * tail sit on separate cache lines to avoid false sharing; ordering is enforced
 * with acquire/release on the index the other side owns. */
#define IPC_RING_CAP 256                  /* must be a power of two */

struct ipc_ring {
    uint64_t res_id;
    _Alignas(64) uint32_t head;           /* consumer owns (writes); producer reads */
    _Alignas(64) uint32_t tail;           /* producer owns (writes); consumer reads */
    _Alignas(64) uint64_t buf[IPC_RING_CAP];
};

void ipc_ring_init(struct ipc_ring *r, uint64_t res_id);
/* push: 0 ok, 1 full, -1 cap denied.  pop: 0 ok, 1 empty, -1 cap denied. */
int  ipc_ring_push(struct cap_table *caps, cap_t h, struct ipc_ring *r, uint64_t val);
int  ipc_ring_pop(struct cap_table *caps, cap_t h, struct ipc_ring *r, uint64_t *out);
