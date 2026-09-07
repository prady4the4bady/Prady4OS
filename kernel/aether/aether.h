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
    ACTION_NET_CONNECT,
    /* DDR-842 (3C). APPENDED, never inserted: action_type crosses the ring
     * boundary in every audit record and queue entry, so an insertion renumbers
     * a wire format exactly as DDR-832 describes for enum aether_result.
     *
     * Six further 3C types are deliberately ABSENT until their subsystem exists
     * — CAPTURE_FRAME / SCAN_ENVIRONMENT / QUERY_SCENE (post-L7, CAP_SCENE),
     * PARSE_DOCUMENT (64 MiB OCR model), EXEC_CODE (sandboxed interpreter),
     * BROWSE_WEB (headless browser + deferred cloud bridge). Declaring an enum
     * value with no enforcement is worse than omitting it: an agent could submit
     * one and the kernel would queue an action nothing implements. */
    ACTION_READ_FILE, ACTION_DELETE_FILE, ACTION_SEND_IPC, ACTION_QUERY_MEMORY,
    ACTION_REWRITE_AGENT_CODE, ACTION_PROPOSE_HYPOTHESIS,
    ACTION_RUN_EXPERIMENT, ACTION_EVOLVE_GENOME,
    /* DDR-1070: egress THROUGH an already-open proxy socket, as distinct from
     * opening one. Appended, never inserted, per this enum's own rule above.
     * It exists so the audit trail can say WHICH operation privacy mode
     * refused: reusing ACTION_NET_CONNECT would have recorded a blocked write
     * as a blocked connect, and DDR-801's rule is that the record states the
     * decision that was actually made. Audit-only -- nothing SUBMITS this type,
     * so it never reaches the approval path and aether_action_forces_pending()
     * is deliberately unchanged. */
    ACTION_NET_EGRESS
};

/* Wire-format pins for the pre-existing action types (DDR-832 discipline). */
_Static_assert(ACTION_WRITE_FILE    == 1, "action wire format: WRITE_FILE is 1");
_Static_assert(ACTION_PRINT         == 2, "action wire format: PRINT is 2");
/* SPAWN_PROCESS is also hand-copied by user/actionspawntest.c (DDR-1017), whose
 * gate asserts it stays PENDING -- so a drift onto a type outside
 * aether_action_forces_pending() below would make that assertion vacuous. */
_Static_assert(ACTION_SPAWN_PROCESS == 3, "action wire format: SPAWN_PROCESS is 3");
/* DDR-1015: pin the first 3C type too. user/actionreadtest.c hand-copies this
 * number across the ring boundary, and DDR-1013 §1 found actiondagtest.c had
 * drifted to a wrong one with no gate able to see it. Pinning it here means the
 * KERNEL stops building if the enum shifts, which is the only cross-check the
 * build currently has between a probe's constants and this header. */
_Static_assert(ACTION_READ_FILE     == 5, "action wire format: READ_FILE is 5");
/* DDR-1016: and the first force-pending 3C type. user/actiondeltest.c hand-copies
 * this one, and its gate asserts the action stays PENDING -- an assertion that
 * would silently become vacuous if the number drifted onto a type that is not in
 * aether_action_forces_pending() below. */
_Static_assert(ACTION_DELETE_FILE   == 6, "action wire format: DELETE_FILE is 6");
/* DDR-1018 recorded that ACTION_SEND_IPC == 7 was deliberately NOT pinned:
 * ipc_send/ipc_recv were kernel-internal with no SYS_IPC_*, so an approved
 * SEND_IPC had no executor in any ring (DDR-1017 §1) -- and the rule it gave is
 * the one this file lives by: "Nothing hand-copies 7, and A PIN WHOSE PROBE DOES
 * NOT EXIST WOULD READ AS A CLAIM THAT ONE DOES."
 *
 * PINNED 2026-09-07 (DDR-1084 §2), because the rule's CONDITION is now
 * satisfied. DDR-1033 built the door; user/actionipctest.c is the caller, and it
 * hand-copies 7 and submits the type. Measured before the pin was added: the
 * only two matches for ACTION_SEND_IPC outside this header were COMMENTS
 * (user/actionquerytest.c:4, user/ipctest.c:3), so 7 genuinely crossed no ring
 * boundary until that probe existed.
 *
 * Worth reading beside the RUN_EXPERIMENT note below, which DDR-1083 §2 found
 * carrying the same claim in the confident past tense about a probe
 * (user/exptest.c) that does not hand-copy anything. Same file, same rule, one
 * commit apart, opposite outcomes -- and the only difference is whether the
 * probe was built before or after the sentence asserting it. */
_Static_assert(ACTION_SEND_IPC      == 7, "action wire format: SEND_IPC is 7");
_Static_assert(ACTION_QUERY_MEMORY  == 8, "action wire format: QUERY_MEMORY is 8");
/* DDR-1020. Both hand-copied by user/actionhypotest.c, which runs them in ONE
 * boot on opposite sides of the force-pending split below -- so a drift that
 * moved either onto the wrong side would make that comparison vacuous. 9
 * (REWRITE_AGENT_CODE) is hand-copied by user/coderewritetest.c and pinned for
 * the same reason. 11 (RUN_EXPERIMENT) WAS deliberately unpinned on that rule --
 * nothing copied it, and a pin whose probe does not exist reads as a claim that
 * one does.
 *
 * CORRECTED 2026-09-07 (DDR-1083 §2). This comment used to name user/exptest.c
 * as that probe. IT IS NOT ONE: measured, exptest.c hand-copies four NSI numbers
 * (4/6/100/101) and the exp_op opcodes, contains no ACTION_ constant at all and
 * never calls SYS_SUBMIT_ACTION -- it drives the EXECUTOR, which is exactly the
 * distinction DDR-1072 §2 drew when it warned that smoke-runexp's NAME matches
 * the action type while its CLAIM is a different thing. So this file held a rule
 * (eleven lines up, for SEND_IPC) and a violation of it, written in the
 * confident past tense.
 *
 * The pin is not deleted; it is MADE TRUE. user/actionexptest.c (DDR-1083) hand-
 * copies 11 and submits the type, so the sentence is now accurate rather than
 * the claim being quietly shrunk. */
_Static_assert(ACTION_REWRITE_AGENT_CODE == 9, "action wire format: REWRITE_AGENT_CODE is 9");
_Static_assert(ACTION_PROPOSE_HYPOTHESIS == 10, "action wire format: PROPOSE_HYPOTHESIS is 10");
_Static_assert(ACTION_RUN_EXPERIMENT     == 11, "action wire format: RUN_EXPERIMENT is 11");
_Static_assert(ACTION_EVOLVE_GENOME      == 12, "action wire format: EVOLVE_GENOME is 12");
/* DDR-1070: audit-only, privacy-refused egress on an open socket. */
_Static_assert(ACTION_NET_EGRESS         == 13, "action wire format: NET_EGRESS is 13");

/* DDR-842: never auto-approved, even in sovereign mode (S4 — the human gate is
 * structural). ONE list, used by the queue, so there are not two that must
 * agree. */
static inline int aether_action_forces_pending(uint32_t t) {
    return t == ACTION_SPAWN_PROCESS || t == ACTION_DELETE_FILE ||
           t == ACTION_REWRITE_AGENT_CODE || t == ACTION_EVOLVE_GENOME;
}

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
    AR_PRIVACY_BLOCKED,
    /* DDR-813 ACC. Three distinct codes, not one: an envelope SEALED, an
     * envelope OPENED, and an envelope REJECTED are different operator-facing
     * facts. Folding the rejection into AR_CAP_DENIED would make "was a forged
     * or replayed envelope ever presented?" unanswerable — which is the only
     * question that shows the channel is doing anything. The rejection record
     * carries the ACC_ERR_* code so forgery and replay stay distinguishable. */
    AR_ACC_SEALED, AR_ACC_OPENED, AR_ACC_REJECTED,
    /* DDR-814 AGS. APPENDED, never inserted — see the append-only rule below.
     * Kept distinct from AR_APPROVE/AR_CAP_DENIED deliberately: "the owner
     * signed this goal" and "policy allowed this action" are not the same fact,
     * and a REJECTED goal is a forgery attempt rather than a policy refusal.
     * Collapsing either pair makes "was a forged goal ever presented?"
     * unanswerable from the log — the only question that shows AGS works. */
    AR_GOAL_SIGNED, AR_GOAL_REJECTED,
    /* DDR-815. APPENDED per DDR-832, never inserted. Distinct because a rotation
     * is the event that EXPLAINS why previously-valid envelopes stopped
     * verifying; folded into AR_APPROVE that becomes unexplainable. */
    AR_ACC_ROTATED,
    /* DDR-834. APPENDED per DDR-832. AR_VAULT_REJECTED is distinct from
     * AR_CAP_DENIED because a rejected TAG means the stored bytes were tampered
     * with on disk — an attack on the image — while a denial means policy said
     * no. Collapsing them makes "was the vault ever tampered with?"
     * unanswerable. */
    AR_VAULT_PUT, AR_VAULT_GET, AR_VAULT_REJECTED,
    /* DDR-836. APPENDED per DDR-832. Reads are recorded as well as writes: the
     * store is a shared blackboard, so "who read this fact" is as much an
     * operator question as who wrote it. */
    AR_MEM_WRITE, AR_MEM_READ,
    /* DDR-837. APPENDED per DDR-832. Without these, "why did this agent stop
     * producing?" is unanswerable from the log, and an operator freeze looks
     * exactly like a fault. */
    AR_AGENT_CHECKPOINT, AR_AGENT_RESUME,
    /* DDR-842. APPENDED per DDR-832. AR_AUDIT_TAMPERED is distinct from every
     * other rejection: "the log itself is forged" is not a policy denial, and it
     * is the one record an operator must never see folded into a generic code. */
    AR_CODE_REWRITE_APPROVED, AR_AUDIT_READ, AR_AUDIT_TAMPERED
};

/* DDR-832 — THIS ENUM IS APPEND-ONLY WIRE FORMAT.
 *
 * These values cross the ring boundary inside audit records, and ring-3 probes
 * duplicate them as literals (e.g. user/egressaudittest.c hardcodes
 * AR_SOVEREIGN_BYPASS 9 / AR_NET_CONNECT 10) because a freestanding probe cannot
 * include this header. Inserting a code in the middle silently renumbers every
 * later one and the probes then assert against stale numbers — which is exactly
 * what wiring AGS did: three shards went red on smoke-egress-audit,
 * smoke-privacy-netfilter and smoke-sovereign-egress.
 *
 * Add new codes at the END, and pin every externally-duplicated value here. A
 * renumbering now fails the BUILD instead of a gate three shards away. */
_Static_assert(AR_CAP_DENIED       == 7,  "audit wire format: AR_CAP_DENIED must stay 7");
_Static_assert(AR_SOVEREIGN_BYPASS == 9,  "audit wire format: user/egressaudittest.c hardcodes 9");
_Static_assert(AR_NET_CONNECT      == 10, "audit wire format: user/egressaudittest.c hardcodes 10");
_Static_assert(AR_PRIVACY_BLOCKED  == 11, "audit wire format: privacy-netfilter probe depends on 11");
_Static_assert(AR_ACC_SEALED       == 12, "audit wire format: ACC probe depends on 12");
_Static_assert(AR_ACC_OPENED       == 13, "audit wire format: ACC probe depends on 13");
_Static_assert(AR_ACC_REJECTED     == 14, "audit wire format: ACC probe depends on 14");

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
/* DDR-839: parent_action_id 0 == root. A child cannot be APPROVED until its
 * parent is; see the DDR for why cycles are impossible rather than checked. */
long aether_submit(uint32_t agent_pid, uint32_t action_type,
                   const uint8_t *payload, uint32_t len,
                   uint64_t parent_action_id);
long aether_poll(uint32_t agent_pid, uint64_t action_id);     /* -> status | -ESRCH */
long aether_approve(uint64_t action_id, int approve);          /* approve=1 / reject=0 */
/* DDR-842: the action_type of a live action, or -1 if there is no such action.
 * SYS_APPROVE_CODE_REWRITE uses it to refuse anything that is not a code
 * rewrite, so it cannot serve as a general approval bypass. */
int  aether_action_type_of(uint64_t action_id, uint32_t *type_out);
void aether_drop_pid(uint32_t pid);            /* free an exited agent's queue slots */

/* --- audit log (aether_audit.c) -------------------------------------------- */
struct aether_audit_entry_pub {                /* the shape SYS_READ_AUDIT returns */
    uint64_t timestamp; uint32_t agent_pid; uint32_t action_type;
    uint64_t action_id; uint32_t result;     uint32_t _pad;
};
/* DDR-842: the chain value is deliberately NOT in this struct. NSI 37
 * (SYS_READ_AUDIT) is shipped and user/egressaudittest.c, user/privacynettest.c
 * and user/sovegresstest.c each carry their own copy of this layout. Adding 32
 * bytes per entry would make the kernel write past buffers those probes sized
 * for the old shape — an overflow, not merely a parse error. Verification is a
 * SEPARATE call (SYS_VERIFY_AUDIT, 93) that returns a verdict, not records. */

/* DDR-842: recompute the chain over the RETAINED window and report the index of
 * the first mismatch. Returns 0 when intact, -1 when broken (*bad_index set).
 * A boolean would be useless to an operator: "tampered at entry 1204" locates
 * the event being hidden; "tampered" does not. */
int  aether_audit_verify(uint32_t *bad_index);
/* DDR-842 fault injection, DDR-804 probe-gated. Flips one byte of one committed
 * entry so the gate can prove verification FAILS when it should. Ring 3 has no
 * write path into the log (that is what S5 asserts), so this is the only way to
 * exercise the failure branch. */
void aether_audit_tamper(void);
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
