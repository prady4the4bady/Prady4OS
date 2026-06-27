/* kernel/aether/aether_audit.c — AETHER append-only circular audit log.
 *
 * 4096 fixed entries in kernel BSS. Append-only from user space (no erase/rewrite
 * syscall exists). On wrap we emit a single AETHER_AUDIT_WRAP event to serial so
 * the loss of the oldest records is itself auditable (ADR-026 D4).
 */
#include "aether.h"
#include "string.h"
#include "console.h"
#include "irq.h"      /* g_ticks */
#include "pmm.h"      /* big tables come from the PMM pool, not BSS (low-mem cap) */

struct aether_audit_entry {
    uint64_t timestamp;
    uint32_t agent_pid;
    uint32_t action_type;
    uint64_t action_id;
    uint32_t result;
    uint32_t _pad;
};

/* 4096-entry ring (128 KiB) — PMM-pool allocated (see aether_queue.c rationale). */
static struct aether_audit_entry *g_log;
static uint32_t g_head;          /* next write index */
static uint32_t g_count;         /* total appended (caps at AETHER_AUDIT_LEN live) */
static int      g_wrapped;       /* 1 once the ring has wrapped at least once */

void aether_audit_init(void) {
    g_log = (struct aether_audit_entry *)(uintptr_t)pmm_alloc_pages(5); /* 32 pg = 128 KiB */
    if (g_log)
        memset(g_log, 0, AETHER_AUDIT_LEN * sizeof *g_log);
}

void aether_audit(uint32_t agent_pid, uint32_t action_type,
                  uint64_t action_id, uint32_t result) {
    if (!g_log)
        return;
    struct aether_audit_entry *e = &g_log[g_head];
    e->timestamp   = g_ticks;
    e->agent_pid   = agent_pid;
    e->action_type = action_type;
    e->action_id   = action_id;
    e->result      = result;
    e->_pad        = 0;

    g_head = (g_head + 1) % AETHER_AUDIT_LEN;
    if (g_head == 0) {
        if (!g_wrapped) g_wrapped = 1;
        kputs("AETHER_AUDIT_WRAP\r\n");        /* oldest records now overwritten */
    }
    if (g_count < AETHER_AUDIT_LEN)
        g_count++;
}

/* Copy up to `max` entries oldest..newest into a kernel-side buffer. The caller
 * (SYS_READ_AUDIT) then copyout()s to user space. */
int aether_audit_read(struct aether_audit_entry_pub *out, int max) {
    if (max <= 0 || !g_log)
        return 0;
    int n = (int)g_count;
    if (n > max) n = max;
    /* Oldest entry: if wrapped, it's at g_head; else at 0. Walk forward n from the
     * (newest - n) position so we return the most recent n in chronological order. */
    uint32_t start = (g_head + AETHER_AUDIT_LEN - (uint32_t)n) % AETHER_AUDIT_LEN;
    for (int i = 0; i < n; i++) {
        const struct aether_audit_entry *e = &g_log[(start + (uint32_t)i) % AETHER_AUDIT_LEN];
        out[i].timestamp   = e->timestamp;
        out[i].agent_pid   = e->agent_pid;
        out[i].action_type = e->action_type;
        out[i].action_id   = e->action_id;
        out[i].result      = e->result;
        out[i]._pad        = 0;
    }
    return n;
}
