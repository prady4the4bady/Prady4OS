/* user/coderewritetest.c — DDR-842 code-rewrite approval gate (NSI 86).
 *
 * FOUR ROLES from one ELF, each self-identified at runtime. The capability
 * split is the entire feature, and no single privilege level can test it.
 *
 *   agent     (CAP_AGENT+MEMORY)            submits ACTION_REWRITE_AGENT_CODE
 *   approver  (SOVEREIGN+REWRITE+MEMORY)    approves it -> 0
 *   sov-only  (SOVEREIGN+MEMORY)            approve -> -EPERM
 *   rw-only   (REWRITE only)                approve -> -EPERM
 *
 * THE sov-only ARM IS THE POINT. If CAP_SOVEREIGN alone were enough, CAP_REWRITE
 * would be decoration and every sovereign process would silently be a
 * code-rewrite authority. A gate that only tested "unprivileged is denied" would
 * pass against exactly that bug.
 *
 * Role detection, in order, using what each role can uniquely do:
 *   submit succeeds            -> agent
 *   approve(0) is -EINVAL      -> approver (both bits: it got past the cap check)
 *   verify_audit is not -EPERM -> sov-only (sovereign, but approve was refused)
 *   otherwise                  -> rw-only
 *
 * NO WRITABLE GLOBALS (DDR-826): one R+X PT_LOAD, everything on the stack.
 */
#include <stdint.h>

#define SYS_WRITE                 6
#define SYS_EXIT                  4
#define SYS_YIELD                 3
#define SYS_SET_MODE             30
#define SYS_SUBMIT_ACTION        31
#define SYS_MEMORY_WRITE         82
#define SYS_MEMORY_READ          83
#define SYS_APPROVE_CODE_REWRITE 86
#define SYS_VERIFY_AUDIT         93

/* Must match enum aether_action: NONE,WRITE_FILE,PRINT,SPAWN_PROCESS,
 * NET_CONNECT,READ_FILE,DELETE_FILE,SEND_IPC,QUERY_MEMORY,REWRITE_AGENT_CODE */
#define ACTION_PRINT              2
#define ACTION_REWRITE_AGENT_CODE 9
#define AETHER_MODE_MANUAL        0

#define EPERM   1
#define EINVAL 22

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}
static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

static void wrint(long v) {
    char b[24]; int i = 23; int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    b[i--] = 0;
    if (!u) b[i--] = '0';
    while (u) { b[i--] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) b[i--] = '-';
    wr(&b[i + 1]);
}
static void fail(const char *why, long rc) {
    wr("CODEREWRITE FAIL: "); wr(why); wr(" rc="); wrint(rc); wr("\r\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}
static void spin_delay(void) { for (volatile long i = 0; i < 200000; i++) { } }

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    long sub = nsi(SYS_SUBMIT_ACTION, ACTION_PRINT, (long)"probe", 5);

    if (sub >= 0) {
        /* ---- agent: submit the rewrite action and publish its id ---- */
        long id = nsi(SYS_SUBMIT_ACTION, ACTION_REWRITE_AGENT_CODE, (long)"patch", 5);
        if (id < 0)
            fail("submitting ACTION_REWRITE_AGENT_CODE", id);

        uint64_t v = (uint64_t)id;
        if (nsi(SYS_MEMORY_WRITE, (long)"CRW_ID", (long)&v, sizeof v) != 0)
            fail("publishing the action id", 0);

        wr("PRADYOS_CODEREWRITE_SUBMIT_OK\r\n");
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }

    long probe = nsi(SYS_APPROVE_CODE_REWRITE, 0, 0, 0);

    if (probe == -EINVAL) {
        /* ---- approver: holds BOTH bits (it reached the action_id check) ---- */
        uint64_t id = 0; uint32_t got = 0;
        int have = 0;
        for (int i = 0; i < 400 && !have; i++) {
            if (nsi(SYS_MEMORY_READ, (long)"CRW_ID", (long)&id, (long)&got) == 0 &&
                got == sizeof id)
                have = 1;
            else { spin_delay(); nsi(SYS_YIELD, 0, 0, 0); }
        }
        if (!have)
            fail("agent never published the action id", (long)got);

        /* A non-rewrite action must be refused even with both bits — otherwise
         * this call is a general-purpose approval bypass wearing a name. */
        long other = nsi(SYS_SUBMIT_ACTION, ACTION_PRINT, (long)"x", 1);
        if (other >= 0) {
            long bad = nsi(SYS_APPROVE_CODE_REWRITE, other, 0, 0);
            if (bad == 0)
                fail("approved a NON-rewrite action", bad);
        }

        long ok = nsi(SYS_APPROVE_CODE_REWRITE, (long)id, 0, 0);
        if (ok != 0)
            fail("approving the rewrite action with both bits", ok);

        wr("PRADYOS_CODEREWRITE_OK\r\n");
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }

    if (probe != -EPERM)
        fail("unexpected probe result", probe);

    long va = nsi(SYS_VERIFY_AUDIT, 0, 0, 0);
    if (va != -EPERM) {
        /* ---- sovereign but NOT rewrite: the arm that matters ---- */
        wr("PRADYOS_CODEREWRITE_SOVONLY_DENIED_OK\r\n");
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }

    /* ---- rewrite bit only, no sovereignty ---- */
    wr("PRADYOS_CODEREWRITE_RWONLY_DENIED_OK\r\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
