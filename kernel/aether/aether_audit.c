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
#include "sha256.h"   /* DDR-842: the audit chain */
#include "spinlock.h" /* DDR-842: the chain makes appends interdependent */
#define CRLF "\r\n"

struct aether_audit_entry {
    uint64_t timestamp;
    uint32_t agent_pid;
    uint32_t action_type;
    uint64_t action_id;
    uint32_t result;
    uint32_t _pad;
    /* DDR-842: chain[i] = SHA-256(chain[i-1] || the six fields above).
     * Append-only removes the user-space write path; it says NOTHING about the
     * bytes on the page. Only a recomputable chain distinguishes an intact log
     * from an edited one. */
    uint8_t  chain[32];
};

/* Hash the six payload fields of `e` onto `prev`, producing `out`. Field-by-field
 * rather than hashing the struct: struct padding is not guaranteed zeroed, and a
 * chain over uninitialised padding would fail verification for no reason. */
static void chain_step(const uint8_t prev[32],
                       const struct aether_audit_entry *e,
                       uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, prev, 32);
    sha256_update(&c, &e->timestamp,   sizeof e->timestamp);
    sha256_update(&c, &e->agent_pid,   sizeof e->agent_pid);
    sha256_update(&c, &e->action_type, sizeof e->action_type);
    sha256_update(&c, &e->action_id,   sizeof e->action_id);
    sha256_update(&c, &e->result,      sizeof e->result);
    sha256_final(&c, out);
}

/* 4096-entry ring (128 KiB) — PMM-pool allocated (see aether_queue.c rationale). */
static struct aether_audit_entry *g_log;
static uint32_t g_head;          /* next write index */
static uint32_t g_count;         /* total appended (caps at AETHER_AUDIT_LEN live) */
static int      g_wrapped;       /* 1 once the ring has wrapped at least once */

/* DDR-842. Before the hash chain, appends were INDEPENDENT — each entry was a
 * self-contained record, so two concurrent writers could not corrupt each
 * other's data and no lock was needed. The chain changed that: an append now
 * READS the previous entry's chain value and WRITES its own from it, and two
 * interleaved appends chain off the same predecessor, leaving the second entry
 * unverifiable.
 *
 * That is not hypothetical — it is what the first run of smoke-auditchain
 * found: verification reported tampering at index 1810 of an untouched log.
 * Adding a cross-entry dependency to a lock-free structure requires adding the
 * lock with it. */
static spinlock_t g_audit_lock = SPINLOCK_INIT;

/* DDR-842: the order is DERIVED from the struct, never a literal.
 *
 * This was a real bug, not a tidy-up. The ring was allocated with a hardcoded
 * pmm_alloc_pages(5) = 128 KiB, correct for a 32-byte entry. Adding chain[32]
 * grew the entry to 64 bytes, so 4096 entries need 256 KiB — and the log spent
 * every boot writing 128 KiB PAST its allocation into whatever the PMM had
 * handed out next. It surfaced as verification reporting tampering at index
 * 1810: the second half of the ring was living in memory that was not ours.
 *
 * A literal page count that encodes sizeof(struct) is a constant that goes stale
 * the moment the struct changes — the same family as DDR-831's LBA and DDR-838's
 * inherited-flag assumption. Computing the order here means the allocation
 * cannot disagree with the type, and the _Static_assert below fails the BUILD if
 * the struct ever outgrows the computed order. */
#define AUDIT_RING_BYTES (AETHER_AUDIT_LEN * sizeof(struct aether_audit_entry))
#define AUDIT_RING_ORDER 6u                       /* 64 pages = 256 KiB */
_Static_assert(AUDIT_RING_BYTES <= (4096u << AUDIT_RING_ORDER),
               "audit ring does not fit its allocation order — raise AUDIT_RING_ORDER");
_Static_assert(AUDIT_RING_BYTES >  (4096u << (AUDIT_RING_ORDER - 1u)),
               "audit ring order is larger than needed — lower AUDIT_RING_ORDER");

void aether_audit_init(void) {
    g_log = (struct aether_audit_entry *)(uintptr_t)pmm_alloc_pages(AUDIT_RING_ORDER);
    if (g_log)
        memset(g_log, 0, AUDIT_RING_BYTES);
}

void aether_audit(uint32_t agent_pid, uint32_t action_type,
                  uint64_t action_id, uint32_t result) {
    if (!g_log)
        return;
    uint64_t fl = spin_lock_irqsave(&g_audit_lock);
    struct aether_audit_entry *e = &g_log[g_head];
    e->timestamp   = g_ticks;
    e->agent_pid   = agent_pid;
    e->action_type = action_type;
    e->action_id   = action_id;
    e->result      = result;
    e->_pad        = 0;

    /* Chain onto the previous entry's value. The first entry of the boot chains
     * onto 32 zero bytes. */
    {
        uint8_t prev[32];
        if (g_count == 0) {
            memset(prev, 0, sizeof prev);
        } else {
            uint32_t p = (g_head + AETHER_AUDIT_LEN - 1) % AETHER_AUDIT_LEN;
            memcpy(prev, g_log[p].chain, 32);
        }
        chain_step(prev, e, e->chain);
    }

    g_head = (g_head + 1) % AETHER_AUDIT_LEN;
    if (g_head == 0) {
        if (!g_wrapped) g_wrapped = 1;
        kputs("AETHER_AUDIT_WRAP\r\n");        /* oldest records now overwritten */
    }
    if (g_count < AETHER_AUDIT_LEN)
        g_count++;
    spin_unlock_irqrestore(&g_audit_lock, fl);
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

/* DDR-842: recompute the chain across the retained window.
 *
 * HONEST LIMIT, stated because it would otherwise look stronger than it is: this
 * is a CIRCULAR buffer, so verification covers what is retained, not all history.
 * After a wrap the oldest retained entry has no in-log predecessor, so the walk
 * is ANCHORED at its stored chain value rather than at a genesis constant — a
 * wrap is a real gap in the chain of custody, which is why AETHER_AUDIT_WRAP is
 * emitted to serial. The durable ledger that survives wrap is F#76, a different
 * mechanism, not claimed here.
 */
int aether_audit_verify(uint32_t *bad_index) {
    if (!g_log)
        return 0;
    /* Hold the append lock for the walk. A concurrent append during
     * verification would move g_head under us and the walk would compare an
     * entry against a predecessor it was never chained to — reporting tampering
     * that never happened, which is worse than missing real tampering because
     * it destroys trust in the verifier. */
    uint64_t fl = spin_lock_irqsave(&g_audit_lock);
    if (g_count == 0) {
        spin_unlock_irqrestore(&g_audit_lock, fl);
        return 0;                       /* nothing retained: vacuously intact */
    }

    uint32_t n = g_count;
    uint32_t start = (g_head + AETHER_AUDIT_LEN - n) % AETHER_AUDIT_LEN;

    uint8_t prev[32];
    memcpy(prev, g_log[start].chain, 32);   /* anchor, see the note above */

    for (uint32_t i = 1; i < n; i++) {
        const struct aether_audit_entry *e =
            &g_log[(start + i) % AETHER_AUDIT_LEN];
        uint8_t want[32];
        chain_step(prev, e, want);
        for (int b = 0; b < 32; b++) {
            if (want[b] != e->chain[b]) {
                if (bad_index) *bad_index = i;
                spin_unlock_irqrestore(&g_audit_lock, fl);
                return -1;
            }
        }
        memcpy(prev, e->chain, 32);
    }
    spin_unlock_irqrestore(&g_audit_lock, fl);
    return 0;
}

/* DDR-842 fault injection, DDR-804 probe-gated by the caller. Ring 3 has no write
 * path into this log — that is what S5 asserts — so this is the only way to prove
 * aether_audit_verify() can actually FAIL. A gate that only ever verifies an
 * intact log would pass against a verify() that returns OK unconditionally. */
void aether_audit_tamper(void) {
    if (!g_log)
        return;
    uint64_t fl = spin_lock_irqsave(&g_audit_lock);
    if (g_count < 3) {
        spin_unlock_irqrestore(&g_audit_lock, fl);
        return;
    }
    /* Corrupt a RECENT entry, not the oldest.
     *
     * The first version targeted the oldest retained record, and the tamper gate
     * failed: injection happens at probe-spawn time, the boot then appends
     * thousands more audit records, the ring wraps, and the corrupted entry is
     * overwritten before verification ever runs — so the log verified clean and
     * the gate reported no tampering. Targeting the second-newest entry keeps the
     * corruption inside the retained window for another ~4094 appends, which is
     * the whole remaining boot.
     *
     * That failure was worth having: it is the same "the evidence expired before
     * anyone looked" property that makes a wrap a real gap in the chain of
     * custody, demonstrated on purpose. */
    struct aether_audit_entry *e =
        &g_log[(g_head + AETHER_AUDIT_LEN - 2u) % AETHER_AUDIT_LEN];
    e->result ^= 0x40;                  /* one bit of one committed record */
    spin_unlock_irqrestore(&g_audit_lock, fl);
    kputs("[audit] TAMPER INJECTED at retained index 1 (probe-gated)\r\n");
}
