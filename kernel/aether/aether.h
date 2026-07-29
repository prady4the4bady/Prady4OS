/* kernel/aether/aether.h — Layer 6 (AETHER) shared types + API (ADR-026/DDR).
 *
 * AETHER lets an untrusted ring-3 *agent* propose consequential actions that the
 * kernel arbitrates. Everything here is bounded and kernel-owned: a fixed action
 * queue, an append-only audit ring, a per-process memory cap, and a syscall rate
 * limit. No allocation in any hot/flooded path; every bound returns an error or a
 * clean process kill — never a kernel panic.
 */
#pragma once
#include <stdint.h>

/* --- action queue ---------------------------------------------------------- */
enum aether_status {
    AE_FREE = 0, AE_PENDING, AE_APPROVED, AE_REJECTED, AE_EXPIRED, AE_DONE
};
enum aether_action {
    ACTION_NONE = 0, ACTION_WRITE_FILE, ACTION_PRINT, ACTION_SPAWN_PROCESS,
    /* DDR-800/801: an outbound socket connect. The destination rides in the
     * record's action_id via AETHER_DEST_ID. */
    ACTION_NET_CONNECT
};

/* audit `result` codes (what happened to an action / a process). */
enum aether_result {
    AR_SUBMIT = 1, AR_APPROVE, AR_REJECT, AR_EXPIRE,
    AR_OOM_KILL, AR_RATE_KILL, AR_CAP_DENIED, AR_WRAP,
    /* DDR-800 (R1): egress proceeded ONLY because the caller is sovereign —
     * it lacked CAP_NET, or the destination was off the allowlist, or both.
     * Distinct from AR_APPROVE on purpose: the entire value of this record is
     * that "allowed by operator authority" and "allowed by policy" are not the
     * same event, and AR_APPROVE would make them indistinguishable. */
    AR_SOVEREIGN_BYPASS,
    /* DDR-801 (R3): a connect that policy allowed, recorded per destination. */
    AR_NET_CONNECT,
    /* DDR-802: refused because privacy mode is on. Deliberately NOT folded into
     * AR_CAP_DENIED: "refused by policy" and "refused because the operator
     * switched egress off" are different operator-facing facts, and collapsing
     * them makes "did privacy mode actually stop anything?" unanswerable — the
     * only question that shows the control works in production, not just in a
     * test. Note this code can carry a SOVEREIGN pid: privacy outranks the
     * DDR-800 bypass, and the attempt is still recorded. */
    AR_PRIVACY_BLOCKED
};

/* DDR-800/801: destination packed into the audit record's action_id. The field
 * is 64-bit and unused on the egress path, so no struct change is needed. */
#define AETHER_DEST_ID(host_be, port)     (((uint64_t)(uint32_t)(host_be) << 16) | (uint64_t)(uint16_t)(port))

#define AETHER_QUEUE_LEN        256
#define AETHER_AUDIT_LEN        4096
#define AETHER_PAYLOAD_MAX      512
#define AETHER_ACTION_TTL_TICKS 6000u          /* 60 s @ 100 Hz PIT */
#define AETHER_MEM_DEFAULT      (128ull << 20) /* 128 MiB per-process cap */
#define AETHER_RATE_MAX         60u            /* syscalls per 1 s window */
#define AETHER_RATE_WINDOW      100u           /* 1 s in PIT ticks */

/* --- init: allocate the queue + audit rings from the PMM pool (call after
 * pmm_init, before any submit/audit). Large static arrays would overrun the
 * low-mem image's page tables (memory note), so these live in the PMM pool. */
void aether_init(void);
void aether_queue_init(void);
void aether_audit_init(void);

/* --- global mode (ADR-026 D2): 1 = sovereign (auto-approve), 0 = manual ----- */
unsigned aether_get_mode(void);
int      aether_set_mode(unsigned mode);       /* caller must hold CAP_SOVEREIGN */

/* DDR-802: privacy mode rides the SAME sovereign-gated SYS_SET_MODE path (no new
 * syscall) as two extra selector values, rather than as a bitmask on the
 * existing argument. A bitmask was rejected: aether_get_mode() is returned
 * verbatim to ring 3 by SYS_GET_MODE, and userspace compares it against 0/1
 * (compositor.c, aether_daemon.c), so OR-ing a privacy bit into it would
 * silently break those comparisons. These values leave the 0/1 contract intact
 * and keep privacy on its own axis — it is not a third kind of sovereignty. */
#define AETHER_MODE_MANUAL       0u
#define AETHER_MODE_SOVEREIGN    1u
#define AETHER_MODE_PRIVACY_ON   2u
#define AETHER_MODE_PRIVACY_OFF  3u

/* Read at call time, never cached: a captured copy goes stale exactly when the
 * operator flips it, which is the moment the control has to be right. */
unsigned aether_privacy_active(void);

/* --- action queue (aether_queue.c) ----------------------------------------- */
/* Submit a copied-in payload; returns a fresh action_id, or <0 errno. */
long aether_submit(uint32_t agent_pid, uint32_t action_type,
                   const uint8_t *payload, uint32_t len);
long aether_poll(uint32_t agent_pid, uint64_t action_id);     /* -> status | -ESRCH */
long aether_approve(uint64_t action_id, int approve);          /* approve=1 / reject=0 */
void aether_drop_pid(uint32_t pid);            /* free an exited agent's queue slots */

/* --- audit log (aether_audit.c) -------------------------------------------- */
struct aether_audit_entry_pub {                /* the shape SYS_READ_AUDIT returns */
    uint64_t timestamp; uint32_t agent_pid; uint32_t action_type;
    uint64_t action_id; uint32_t result;     uint32_t _pad;
};
void aether_audit(uint32_t agent_pid, uint32_t action_type,
                  uint64_t action_id, uint32_t result);
/* Copy up to max entries (oldest..newest) into a kernel-side caller buffer.
 * Returns the number copied. Used by SYS_READ_AUDIT after copyout staging. */
int  aether_audit_read(struct aether_audit_entry_pub *out, int max);

/* --- memory cap + rate limit (aether_mem.c) -------------------------------- */
struct tcb;
/* Charge `bytes` against the process cap; on overflow logs + cleanly kills the
 * process (does not return) and returns -1 conceptually. Returns 0 if allowed. */
int  aether_mem_charge(struct tcb *t, uint64_t bytes);
void aether_mem_uncharge(struct tcb *t, uint64_t bytes);
long aether_set_mem_limit(uint32_t pid, uint64_t bytes);
/* Rate-limit hook: called from the syscall dispatcher for agent processes.
 * Returns 0 to proceed; cleanly kills + does not return when the budget is blown. */
int  aether_rate_check(struct tcb *t);

/* --- agent spawner hook (SYS_SPAWN_AGENT) ---------------------------------- */
/* kmain registers a spawner that elf_loads the embedded agent image and marks it
 * CAP_AGENT; the syscall layer calls it. Returns the new pid, or <0. */
void aether_set_spawn_hook(long (*fn)(const char *task));

/* --- in-boot self-tests (gates) -------------------------------------------- */
void aether_selftest(void);    /* smoke-aether-queue: submit+auto-approve+audit */
void aether_sectest(void);     /* smoke-aether-sec: overflow + audit-wrap paths */
