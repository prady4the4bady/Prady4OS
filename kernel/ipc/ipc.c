/* kernel/ipc/ipc.c — synchronous capability-gated message passing (Phase 2c).
 *
 * DDR-SMP-3c-locks-4: a per-endpoint spinlock (irqsave) guards the condition
 * check + block (receiver) and the deliver + wakeup (sender). The receiver
 * publishes THREAD_BLOCKED *under* the lock via sched_block_on before releasing
 * it, so a sender serialized after the release always sees the waiter — the
 * lost-wakeup race is closed across CPUs, not just locally. context_switch
 * preserves RFLAGS, so a blocked receiver resumes with interrupts still masked
 * and re-checks the condition before the final irqrestore.
 */
#include "ipc.h"
#include "errno.h"     /* DDR-955: ETIMEDOUT */
#include "sched.h"

/* DDR-873 (item 43): the 32-byte payload copy lives in
 * arch/x86_64/ipc_copy.asm as two MOVDQU pairs. XMM rather than YMM on
 * purpose — see the .asm header: a YMM path would need a runtime gate
 * (FXSAVE does not save YMM on pre-AVX CPUs) and a VZEROUPPER whose cost
 * exceeds the three instructions it would save on a copy this small.
 *
 * Only the two HOT paths use it. The init path below stays a plain loop:
 * it zeroes rather than copies, runs once per endpoint, and giving it a
 * vector path would add a second thing to keep in step with
 * IPC_MSG_WORDS for no measurable gain. */
void ipc_copy32(void *dst, const void *src);
_Static_assert(IPC_MSG_WORDS * sizeof(uint64_t) == 32,
               "ipc_copy32 hard-codes 32 bytes; IPC_MSG_WORDS changed");

void ipc_endpoint_init(struct ipc_endpoint *e, uint64_t res_id) {
    e->full = 0;
    e->res_id = res_id;
    e->waiting_receiver = 0;
    e->lock = (spinlock_t)SPINLOCK_INIT;
    for (int i = 0; i < IPC_MSG_WORDS; i++)
        e->msg[i] = 0;
}

int ipc_send(struct cap_table *caps, cap_t h, struct ipc_endpoint *e, const uint64_t *msg) {
    if (!cap_authorize(caps, h, RES_IPC, e->res_id, CAP_IPC_SEND))
        return -1;

    uint64_t flags = spin_lock_irqsave(&e->lock);
    ipc_copy32(e->msg, msg);
    e->full = 1;
    if (e->waiting_receiver) {
        struct tcb *r = e->waiting_receiver;
        e->waiting_receiver = 0;
        sched_unblock(r);
    }
    spin_unlock_irqrestore(&e->lock, flags);
    return 0;
}

int ipc_recv(struct cap_table *caps, cap_t h, struct ipc_endpoint *e, uint64_t *out) {
    if (!cap_authorize(caps, h, RES_IPC, e->res_id, CAP_IPC_RECV))
        return -1;

    uint64_t flags = spin_lock_irqsave(&e->lock);
    while (!e->full) {
        e->waiting_receiver = current_thread;
        /* DDR-961: bounded. -1 still means "cap denied" (the caller above relies
         * on it); a timeout is -ETIMEDOUT, a value this interface never
         * previously returned. 500 ticks matches the two virtio-blk sites
         * DDR-955 shipped green. */
        if (sched_block_timeout(&e->lock, &e->full, 500) == -ETIMEDOUT) {
            /* Clear our own registration before leaving. Nothing else will:
             * ipc_send clears it only on the wake path, so a timed-out receiver
             * that returns and exits would leave e->waiting_receiver pointing at
             * a freed TCB, and the next ipc_send would sched_unblock() through
             * it. That use-after-free -- not the return-code defect -- is the
             * mechanism behind the lwIP #GPs DDR-955 saw. The == current_thread
             * guard matters: a sender may have cleared it and unblocked us on
             * this same pass, and clobbering it would lose another thread's
             * wakeup. */
            if (e->waiting_receiver == current_thread)
                e->waiting_receiver = 0;
            spin_unlock_irqrestore(&e->lock, flags);
            return -ETIMEDOUT;
        }
    }
    ipc_copy32(out, e->msg);
    e->full = 0;
    spin_unlock_irqrestore(&e->lock, flags);
    return 0;
}

/* --- Async lock-free SPSC ring --------------------------------------------- */

void ipc_ring_init(struct ipc_ring *r, uint64_t res_id) {
    r->res_id = res_id;
    r->head = 0;
    r->tail = 0;
    for (int i = 0; i < IPC_RING_CAP; i++)
        r->buf[i] = 0;
}

int ipc_ring_push(struct cap_table *caps, cap_t h, struct ipc_ring *r, uint64_t val) {
    if (!cap_authorize(caps, h, RES_IPC, r->res_id, CAP_IPC_SEND))
        return -1;
    uint32_t tail = r->tail;                              /* sole producer */
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    if ((uint32_t)(tail - head) >= IPC_RING_CAP)
        return 1;                                         /* full */
    r->buf[tail & (IPC_RING_CAP - 1)] = val;
    __atomic_store_n(&r->tail, tail + 1, __ATOMIC_RELEASE);
    return 0;
}

int ipc_ring_pop(struct cap_table *caps, cap_t h, struct ipc_ring *r, uint64_t *out) {
    if (!cap_authorize(caps, h, RES_IPC, r->res_id, CAP_IPC_RECV))
        return -1;
    uint32_t head = r->head;                              /* sole consumer */
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    if (head == tail)
        return 1;                                         /* empty */
    *out = r->buf[head & (IPC_RING_CAP - 1)];
    __atomic_store_n(&r->head, head + 1, __ATOMIC_RELEASE);
    return 0;
}
