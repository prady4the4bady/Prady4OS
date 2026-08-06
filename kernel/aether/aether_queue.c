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
    /* DDR-839: 0 == root. A parent must already exist to be named, and ids are
     * monotonic and never reused this boot, so parent_action_id < action_id
     * ALWAYS holds and a cycle is structurally impossible. There is no
     * cycle-check here because none is reachable — a property of the id
     * allocator, so anyone making ids reusable must revisit this. */
    uint64_t parent_action_id;
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
static unsigned g_privacy_mode   = 0;          /* DDR-802: off unless the operator asks */

void aether_queue_init(void) {
    g_queue = (struct aether_action_entry *)(uintptr_t)pmm_alloc_pages(6); /* 64 pg = 256 KiB */
    if (g_queue)
        memset(g_queue, 0, AETHER_QUEUE_LEN * sizeof *g_queue);
}

unsigned aether_get_mode(void) { return g_sovereign_mode; }
unsigned aether_privacy_active(void) { return g_privacy_mode; }

/* DDR-802: PRIVACY_ON/OFF are handled first and return without touching the
 * sovereign flag — the two are independent axes, and an operator turning egress
 * off must not silently drop the machine out of sovereign mode. Anything else
 * keeps the historical 0/1 coercion, so existing callers are unaffected. */
int aether_set_mode(unsigned mode) {
    if (mode == AETHER_MODE_PRIVACY_ON)  { g_privacy_mode = 1u; return 0; }
    if (mode == AETHER_MODE_PRIVACY_OFF) { g_privacy_mode = 0u; return 0; }
    g_sovereign_mode = mode ? 1u : 0u;
    return 0;
}

/* Lazily expire a PENDING entry whose 60 s TTL has elapsed. */
static void expire_if_due(struct aether_action_entry *e) {
    if (e->status == AE_PENDING &&
        (g_ticks - e->submit_tick) > AETHER_ACTION_TTL_TICKS) {
        e->status = AE_EXPIRED;
        aether_audit(e->agent_pid, e->action_type, e->action_id, AR_EXPIRE);
    }
}

/* DDR-839: find a live entry by id, or NULL. */
static struct aether_action_entry *entry_of(uint64_t action_id) {
    if (!g_queue || action_id == 0)
        return 0;
    for (int i = 0; i < AETHER_QUEUE_LEN; i++) {
        if (g_queue[i].status != AE_FREE && g_queue[i].action_id == action_id)
            return &g_queue[i];
    }
    return 0;
}

long aether_submit(uint32_t agent_pid, uint32_t action_type,
                   const uint8_t *payload, uint32_t len,
                   uint64_t parent_action_id) {
    if (!g_queue)
        return -ENOMEM;
    /* Fail at SUBMIT, not at approval: accepting an action whose parent does not
     * exist would queue something that can never be approved, and the agent
     * would learn that only much later, from a different call. */
    if (parent_action_id != 0 && !entry_of(parent_action_id))
        return -ESRCH;
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
    e->parent_action_id = parent_action_id;
    e->agent_pid   = agent_pid;
    e->action_type = action_type;
    e->submit_tick = g_ticks;
    memset(e->payload, 0, AETHER_PAYLOAD_MAX);
    if (payload && len)
        memcpy(e->payload, payload, len);

    /* DDR-839: auto-approval must respect the dependency, or it bypasses the whole
     * ordering — a child submitted in sovereign mode would be approved instantly,
     * before its parent, which is exactly what the DAG exists to prevent. A child
     * whose parent is not yet APPROVED stays PENDING and is approved later,
     * through aether_approve, which re-checks the same condition.
     *
     * The gate caught this: enforcing the order only in aether_approve looked
     * complete and left the common path (sovereign mode) unguarded. */
    int parent_ready = (parent_action_id == 0);
    if (!parent_ready) {
        struct aether_action_entry *par = entry_of(parent_action_id);
        parent_ready = (par && par->status == AE_APPROVED);
    }

    /* DDR-842: the force-PENDING set (spawn, delete, code-rewrite, genome) is
     * never auto-approved even in sovereign mode — S4 names each as needing a
     * human gate. One predicate, so the queue and the spec cannot drift. */
    if (!aether_action_forces_pending(action_type) && g_sovereign_mode && parent_ready) {
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
int aether_action_type_of(uint64_t action_id, uint32_t *type_out) {
    struct aether_action_entry *e = entry_of(action_id);
    if (!e)
        return -1;
    if (type_out)
        *type_out = e->action_type;
    return 0;
}

long aether_approve(uint64_t action_id, int approve) {
    if (!g_queue)
        return -ESRCH;
    for (int i = 0; i < AETHER_QUEUE_LEN; i++) {
        struct aether_action_entry *e = &g_queue[i];
        if (e->status == AE_FREE || e->action_id != action_id)
            continue;
        if (e->status != AE_PENDING)
            return -EINVAL;                    /* already decided/expired */
        /* DDR-839: dependency ordering. -EAGAIN, not -EPERM — the operator HAS
         * the authority; the dependency is simply unmet. A rejected or expired
         * parent needs no cascade code: its children can never pass this check,
         * and a cascade would be a second mechanism that has to agree with this
         * one. Only approvals are ordered; a child may always be REJECTED. */
        if (approve && e->parent_action_id != 0) {
            struct aether_action_entry *par = entry_of(e->parent_action_id);
            if (!par || par->status != AE_APPROVED)
                return -EAGAIN;
        }
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
