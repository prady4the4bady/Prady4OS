/* kernel/aether/aether_queue.c — AETHER action queue + sovereign-mode flag.
 *
 * A fixed 256-entry ring in kernel BSS (no allocation). Agents submit copied-in
 * payloads; the kernel auto-approves in sovereign mode or holds PENDING for a
 * human in manual mode. Process-spawn always holds PENDING (ADR-026 D8). Every
 * bound returns an errno; nothing here can panic the kernel.
 */
#include "aether.h"
#include "string.h"
#include "irq.h"      /* g_ticks */
#include "errno.h"
#include "pmm.h"      /* big tables come from the PMM pool, not BSS (low-mem cap) */

/* The internal entry (kernel-private; SYS_READ_AUDIT never exposes this). */
struct aether_action_entry {
    uint64_t action_id;
    uint32_t agent_pid;
    uint32_t action_type;
    uint32_t status;
    uint32_t _pad;
    uint64_t submit_tick;
    uint8_t  payload[AETHER_PAYLOAD_MAX];
};

/* The 256-entry ring (~139 KiB) is allocated from the PMM pool by aether_init(),
 * NOT placed in BSS: a static array this large bloats the kernel image+BSS and
 * risks overrunning the boot page tables (memory note: big tables come from the
 * PMM pool, not BSS). */
static struct aether_action_entry *g_queue;
static uint64_t g_next_action_id = 1;          /* monotonic; never reused this boot */
static unsigned g_sovereign_mode = 1;          /* ADR-026 D2: default sovereign */

void aether_queue_init(void) {
    g_queue = (struct aether_action_entry *)(uintptr_t)pmm_alloc_pages(6); /* 64 pg = 256 KiB */
    if (g_queue)
        memset(g_queue, 0, AETHER_QUEUE_LEN * sizeof *g_queue);
}

unsigned aether_get_mode(void) { return g_sovereign_mode; }
int aether_set_mode(unsigned mode) { g_sovereign_mode = mode ? 1u : 0u; return 0; }

/* Lazily expire a PENDING entry whose 60 s TTL has elapsed. */
static void expire_if_due(struct aether_action_entry *e) {
    if (e->status == AE_PENDING &&
        (g_ticks - e->submit_tick) > AETHER_ACTION_TTL_TICKS) {
        e->status = AE_EXPIRED;
        aether_audit(e->agent_pid, e->action_type, e->action_id, AR_EXPIRE);
    }
}

long aether_submit(uint32_t agent_pid, uint32_t action_type,
                   const uint8_t *payload, uint32_t len) {
    if (!g_queue)
        return -ENOMEM;
    if (len > AETHER_PAYLOAD_MAX)
        len = AETHER_PAYLOAD_MAX;
    int slot = -1;
    for (int i = 0; i < AETHER_QUEUE_LEN; i++) {
        expire_if_due(&g_queue[i]);
        if (slot < 0 && g_queue[i].status == AE_FREE)
            slot = i;
    }
    if (slot < 0)
        return -EAGAIN;                        /* queue full: no overwrite, no crash */

    struct aether_action_entry *e = &g_queue[slot];
    e->action_id   = g_next_action_id++;
    e->agent_pid   = agent_pid;
    e->action_type = action_type;
    e->submit_tick = g_ticks;
    memset(e->payload, 0, AETHER_PAYLOAD_MAX);
    if (payload && len)
        memcpy(e->payload, payload, len);

    /* Spawning a process is never auto-approved, even in sovereign mode (D8). */
    if (action_type != ACTION_SPAWN_PROCESS && g_sovereign_mode) {
        e->status = AE_APPROVED;
        aether_audit(agent_pid, action_type, e->action_id, AR_APPROVE);
    } else {
        e->status = AE_PENDING;
        aether_audit(agent_pid, action_type, e->action_id, AR_SUBMIT);
    }
    return (long)e->action_id;
}

/* Return the action's status to its owner. Terminal states are latched once then
 * the slot is freed, so an exited/looping agent cannot pin the ring. */
long aether_poll(uint32_t agent_pid, uint64_t action_id) {
    if (!g_queue)
        return -ESRCH;
    for (int i = 0; i < AETHER_QUEUE_LEN; i++) {
        struct aether_action_entry *e = &g_queue[i];
        if (e->status == AE_FREE || e->action_id != action_id)
            continue;
        if (e->agent_pid != agent_pid)
            return -ESRCH;                     /* not your action */
        expire_if_due(e);
        uint32_t st = e->status;
        if (st == AE_APPROVED || st == AE_REJECTED || st == AE_EXPIRED) {
            /* Caller has now seen the terminal verdict; release the slot. */
            e->status = AE_FREE;
        }
        return (long)st;
    }
    return -ESRCH;
}

/* Operator decision (caller already proven to hold CAP_SOVEREIGN). */
long aether_approve(uint64_t action_id, int approve) {
    if (!g_queue)
        return -ESRCH;
    for (int i = 0; i < AETHER_QUEUE_LEN; i++) {
        struct aether_action_entry *e = &g_queue[i];
        if (e->status == AE_FREE || e->action_id != action_id)
            continue;
        if (e->status != AE_PENDING)
            return -EINVAL;                    /* already decided/expired */
        e->status = approve ? AE_APPROVED : AE_REJECTED;
        aether_audit(e->agent_pid, e->action_type, e->action_id,
                     approve ? AR_APPROVE : AR_REJECT);
        return 0;
    }
    return -ESRCH;
}

/* Reclaim any slots an exited agent left behind (called from teardown). */
void aether_drop_pid(uint32_t pid) {
    if (!g_queue)
        return;
    for (int i = 0; i < AETHER_QUEUE_LEN; i++)
        if (g_queue[i].status != AE_FREE && g_queue[i].agent_pid == pid)
            g_queue[i].status = AE_FREE;
}
