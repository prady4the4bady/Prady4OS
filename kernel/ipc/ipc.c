/* kernel/ipc/ipc.c — synchronous capability-gated message passing (Phase 2c).
 *
 * Single CPU for now: a cli/sti critical section around the condition check +
 * block (receiver) and the deliver + wakeup (sender) closes the lost-wakeup
 * race. context_switch preserves each thread's RFLAGS, so a blocked receiver
 * resumes with interrupts still masked and re-checks the condition before sti.
 */
#include "ipc.h"
#include "sched.h"

static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }

void ipc_endpoint_init(struct ipc_endpoint *e, uint64_t res_id) {
    e->full = 0;
    e->res_id = res_id;
    e->waiting_receiver = 0;
    for (int i = 0; i < IPC_MSG_WORDS; i++)
        e->msg[i] = 0;
}

int ipc_send(struct cap_table *caps, cap_t h, struct ipc_endpoint *e, const uint64_t *msg) {
    if (!cap_authorize(caps, h, RES_IPC, e->res_id, CAP_IPC_SEND))
        return -1;

    cli();
    for (int i = 0; i < IPC_MSG_WORDS; i++)
        e->msg[i] = msg[i];
    e->full = 1;
    if (e->waiting_receiver) {
        struct tcb *r = e->waiting_receiver;
        e->waiting_receiver = 0;
        sched_unblock(r);
    }
    sti();
    return 0;
}

int ipc_recv(struct cap_table *caps, cap_t h, struct ipc_endpoint *e, uint64_t *out) {
    if (!cap_authorize(caps, h, RES_IPC, e->res_id, CAP_IPC_RECV))
        return -1;

    cli();
    while (!e->full) {
        e->waiting_receiver = current_thread;
        sched_block();             /* sleep until a sender delivers */
    }
    for (int i = 0; i < IPC_MSG_WORDS; i++)
        out[i] = e->msg[i];
    e->full = 0;
    sti();
    return 0;
}
