/* kernel/aether/aether.c — AETHER in-boot self-tests (the queue + security gates).
 *
 * These run once at boot (IRQ context is irrelevant: pure in-kernel data), prove
 * the bounded-resource invariants deterministically, and print the sentinels the
 * smoke-aether-queue / smoke-aether-sec gates grep for. They never panic and
 * leave the queue/audit empty enough for the real daemon + agent to run.
 */
#include "aether.h"
#include "sched.h"
#include "string.h"
#include "console.h"
#include "irq.h"      /* g_ticks */

#define AE_TEST_PID 0xA37E0000u   /* a pid no real process uses, for self-test slots */

/* Allocate the queue + audit rings from the PMM pool (ADR-026 / low-mem note). */
void aether_init(void) {
    aether_queue_init();
    aether_audit_init();
}

/* smoke-aether-queue: submit -> sovereign auto-approve -> audit entry present. */
void aether_selftest(void) {
    const uint8_t msg[] = "PRADYOS_AETHER_SELFTEST";
    long id = aether_submit(AE_TEST_PID, ACTION_PRINT, msg, sizeof msg, 0);
    if (id < 0) { kputs("[aether] selftest submit failed\r\n"); return; }

    long st = aether_poll(AE_TEST_PID, (uint64_t)id);   /* should be APPROVED */
    struct aether_audit_entry_pub a[4];
    int n = aether_audit_read(a, 4);                    /* at least the approve entry */

    if (st == AE_APPROVED && n > 0)
        kputs("PRADYOS_AETHER_QUEUE_OK\r\n");
    else
        kputs("[aether] selftest FAIL\r\n");
    aether_drop_pid(AE_TEST_PID);
}

/* smoke-aether-sec: every bound returns an error / clean-kill decision, no panic. */
void aether_sectest(void) {
    /* (a) Queue overflow: fill all 256 slots (sovereign => APPROVED, slot held
     *     until polled), the next submit must return -EAGAIN. */
    int got_eagain = 0;
    for (int i = 0; i < AETHER_QUEUE_LEN + 1; i++) {
        long r = aether_submit(AE_TEST_PID, ACTION_PRINT, (const uint8_t *)"x", 1, 0);
        if (r == -11 /* -EAGAIN */) { got_eagain = 1; break; }
    }
    if (got_eagain) kputs("AETHER_SEC_QUEUE_FULL\r\n");
    aether_drop_pid(AE_TEST_PID);                        /* release the test slots */

    /* (b) Audit wrap: drive past 4096 entries; aether_audit prints AETHER_AUDIT_WRAP. */
    for (int i = 0; i < AETHER_AUDIT_LEN + 1; i++)
        aether_audit(AE_TEST_PID, ACTION_PRINT, (uint64_t)i, AR_SUBMIT);

    /* (c) OOM cap: a throwaway agent TCB over its cap -> kill decision + log. */
    struct tcb fake;
    memset(&fake, 0, sizeof fake);
    fake.is_agent = 1; fake.pid = AE_TEST_PID; fake.mem_limit = 4096;
    if (aether_mem_charge(&fake, 8192) < 0)
        kputs("AETHER_SEC_OOM_OK\r\n");                  /* AGENT_OOM_KILLED already logged */

    /* (d) Rate limit: >60 syscalls in one window -> kill decision + log. */
    fake.sc_window_start = g_ticks; fake.sc_count = 0;
    int rate_killed = 0;
    for (int i = 0; i < (int)AETHER_RATE_MAX + 5; i++)
        if (aether_rate_check(&fake) < 0) { rate_killed = 1; break; }
    if (rate_killed) kputs("AETHER_SEC_RATE_OK\r\n");    /* AGENT_RATE_LIMITED logged */

    /* (e) No self-escalation: raising one's own mem cap is rejected. */
    if (aether_set_mem_limit(AE_TEST_PID, (256ull << 20) + AETHER_MEM_DEFAULT) < 0)
        kputs("AETHER_SEC_CAP_DENIED\r\n");

    kputs("PRADYOS_AETHER_SEC_OK\r\n");
}
