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

#define IPC_MSG_WORDS 4

struct tcb;   /* forward decl (defined in proc/sched.h) */

struct ipc_endpoint {
    int         full;                  /* 1 if msg holds an undelivered message */
    uint64_t    res_id;                /* capability resource id of this endpoint */
    uint64_t    msg[IPC_MSG_WORDS];
    struct tcb *waiting_receiver;      /* a receiver blocked here, or NULL */
};

void ipc_endpoint_init(struct ipc_endpoint *e, uint64_t res_id);

/* 0 on success, -1 if the capability does not authorize the operation. */
int  ipc_send(struct cap_table *caps, cap_t h, struct ipc_endpoint *e, const uint64_t *msg);
int  ipc_recv(struct cap_table *caps, cap_t h, struct ipc_endpoint *e, uint64_t *out);
