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
    ACTION_NONE = 0, ACTION_WRITE_FILE, ACTION_PRINT, ACTION_SPAWN_PROCESS
};

/* audit `result` codes (what happened to an action / a process). */
enum aether_result {
    AR_SUBMIT = 1, AR_APPROVE, AR_REJECT, AR_EXPIRE,
    AR_OOM_KILL, AR_RATE_KILL, AR_CAP_DENIED, AR_WRAP
};

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
