/* user/actiondagtest.c — DDR-839 DAG action queue gate (NSI 92).
 *
 * TWO ROLES from one ELF: an AGENT that submits the graph, and a SOVEREIGN that
 * approves it. Submission is CAP_AGENT and approval is CAP_SOVEREIGN, so no
 * single privilege level can drive this gate alone. They rendezvous through the
 * agent memory store (NSI 82/83), which both hold CAP_MEMORY for: the agent
 * publishes the two action ids, the sovereign reads them.
 *
 * Arms:
 *   1. submit a root, then a child naming it            -> both accepted
 *   2. approve the CHILD first                          -> -EAGAIN
 *   3. approve the parent, then the child               -> both 0
 *   4. submit naming a nonexistent parent               -> -ESRCH
 *   5. reject a second parent, then approve its child   -> still refused
 *
 * Arms 2 and 3 are a PAIR. A queue that refuses every child approval passes arm
 * 2 and is useless; one that ignores parents entirely passes arm 3 and enforces
 * nothing. Either alone is a gate that cannot fail for the right reason.
 *
 * NO WRITABLE GLOBALS (DDR-826): one R+X PT_LOAD, everything on the stack.
 */
#include <stdint.h>

#define SYS_WRITE                6
#define SYS_EXIT                 4
#define SYS_YIELD                3
#define SYS_SET_MODE            30
#define SYS_SUBMIT_ACTION       31
#define SYS_APPROVE_ACTION      33
#define SYS_REJECT_ACTION       34
#define SYS_MEMORY_WRITE        82
#define SYS_MEMORY_READ         83
#define SYS_SUBMIT_CHILD_ACTION 92

#define ACTION_PRINT 1
#define AETHER_MODE_MANUAL 0

#define EPERM   1
#define ESRCH   3
#define EAGAIN 11
#define ENOENT  2

static inline long nsi4(long n, long a1, long a2, long a3, long a4) {
    long r;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}
static long nsi(long n, long a1, long a2, long a3) { return nsi4(n, a1, a2, a3, 0); }
static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

static void wrint(long v) {
    char b[24];
    int i = 23;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    b[i--] = 0;
    if (!u) b[i--] = '0';
    while (u) { b[i--] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) b[i--] = '-';
    wr(&b[i + 1]);
}

static void fail(const char *why, long rc) {
    wr("ACTIONDAG FAIL: ");
    wr(why);
    wr(" rc=");
    wrint(rc);
    wr("\r\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

static void spin_delay(void) {
    for (volatile long i = 0; i < 200000; i++) { }
}

/* ---- rendezvous pacing (ADR-026 D7 budget, ADR-036 counting scope) --------
 *
 * An agent gets AETHER_RATE_MAX counted syscalls per AETHER_RATE_WINDOW, and
 * exceeding it is a KILL, not a stall (kernel/syscall/syscall.c). Since ADR-036
 * SYS_YIELD is free, so one poll of this rendezvous costs exactly ONE counted
 * syscall: the SYS_MEMORY_READ. The poll interval is therefore derived from the
 * kernel's own constants rather than picked — polling at half the budget leaves
 * a 2x margin for the handful of submit syscalls that follow.
 *
 * The wait itself must cost nothing, so it reads the vDSO clock (ring-3
 * readable, no syscall) and yields (free) until the interval has elapsed. */
#define RATE_MAX_SYSCALLS  60u              /* AETHER_RATE_MAX   */
#define RATE_WINDOW_NS     1000000000ull    /* AETHER_RATE_WINDOW */
#define POLL_INTERVAL_NS   (RATE_WINDOW_NS / (RATE_MAX_SYSCALLS / 2u))   /* 30/s */

#define VDSO_USER_VA 0x00007FFFFFF00000ull
static uint64_t vdso_ns(void) { return *(volatile uint64_t *)VDSO_USER_VA; }

/* Yield until the next poll is due. Costs no counted syscalls. */
static void pace_to(uint64_t due_ns) {
    while (vdso_ns() < due_ns)
        nsi(SYS_YIELD, 0, 0, 0);
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    /* Role probe: submitting is CAP_AGENT, so the sovereign gets -EPERM here. */
    long who = nsi(SYS_SUBMIT_ACTION, ACTION_PRINT, (long)"probe", 5);

    if (who >= 0) {
        /* ---------------- agent: build the graph ----------------
         * The probe call above already submitted one action, which is harmless.
         * WAIT for the sovereign to switch to MANUAL mode first: in sovereign
         * mode every action is auto-approved at submit, so the ordering this
         * gate exists to test would never be exercised. The rendezvous is the
         * agent memory store, which both roles hold CAP_MEMORY for. */
        /* Phase breadcrumb: with both roles racing on a loaded boot, a silent
         * agent is indistinguishable from one that was never scheduled. */
        wr("ACTIONDAG AGENT alive\r\n");

        /* DDR-915: poll until the signal arrives. NO internal iteration cap —
         * the harness TIMEOUT_S is the single authority on how long this gate
         * may wait. A nested 400-iteration budget made a SLOW peer read as a
         * BROKEN one, and any replacement count would be equally unmeasured. */
        char go[8];
        uint32_t golen = 0;
        int spins = 0;
        uint64_t due = vdso_ns();
        while (nsi(SYS_MEMORY_READ, (long)"DAG_GO", (long)go, (long)&golen) != 0) {
            if (++spins % 16 == 0) {          /* keeps a timeout diagnosable */
                wr("ACTIONDAG AGENT waiting DAG_GO spins=");
                wrint(spins);
                wr("\r\n");
            }
            due += POLL_INTERVAL_NS;
            pace_to(due);
        }
        wr("ACTIONDAG AGENT go spins=");
        wrint(spins);
        wr("\r\n");

        long root = nsi(SYS_SUBMIT_ACTION, ACTION_PRINT, (long)"root", 4);
        if (root < 0)
            fail("submit root", root);

        long child = nsi4(SYS_SUBMIT_CHILD_ACTION, ACTION_PRINT, (long)"child", 5, root);
        if (child < 0)
            fail("submit child naming a live parent", child);

        /* Arm 4: a parent that does not exist must be refused at SUBMIT time. */
        long orphan = nsi4(SYS_SUBMIT_CHILD_ACTION, ACTION_PRINT, (long)"orphan", 6, 999999);
        if (orphan != -ESRCH)
            fail("submit naming a nonexistent parent was not -ESRCH", orphan);

        /* Arm 5 setup: a second parent/child pair, the parent to be REJECTED. */
        long rroot = nsi(SYS_SUBMIT_ACTION, ACTION_PRINT, (long)"rroot", 5);
        if (rroot < 0)
            fail("submit second root", rroot);
        long rchild = nsi4(SYS_SUBMIT_CHILD_ACTION, ACTION_PRINT, (long)"rchild", 6, rroot);
        if (rchild < 0)
            fail("submit second child", rchild);

        /* Publish the ids for the sovereign. */
        uint64_t ids[4];
        ids[0] = (uint64_t)root; ids[1] = (uint64_t)child;
        ids[2] = (uint64_t)rroot; ids[3] = (uint64_t)rchild;
        if (nsi(SYS_MEMORY_WRITE, (long)"DAG_IDS", (long)ids, sizeof ids) != 0)
            fail("publishing action ids", 0);

        wr("PRADYOS_ACTIONDAG_SUBMIT_OK\r\n");
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }

    /* ---------------- sovereign: approve in and out of order ---------------- */
    if (who != -EPERM)
        fail("sovereign submit was not -EPERM", who);

    /* MANUAL mode first: sovereign mode auto-approves at submit, which would
     * decide both actions before this role ever runs and make arms 2-3 vacuous. */
    long sm = nsi(SYS_SET_MODE, AETHER_MODE_MANUAL, 0, 0);
    if (sm != 0)
        fail("switching to MANUAL mode", sm);
    if (nsi(SYS_MEMORY_WRITE, (long)"DAG_GO", (long)"1", 1) != 0)
        fail("signalling the agent", 0);

    uint64_t ids[4];
    uint32_t got = 0;
    int have = 0;
    wr("ACTIONDAG SOV waiting\r\n");
    /* DDR-915: same rule as the agent side — poll until the ids appear; the
     * harness timeout bounds the gate. This wait is what previously expired
     * first under host load and reported a live agent as one that never ran. */
    int sspins = 0;
    while (!have) {
        if (nsi(SYS_MEMORY_READ, (long)"DAG_IDS", (long)ids, (long)&got) == 0 &&
            got == sizeof ids)
            have = 1;
        else {
            if (++sspins % 64 == 0) {
                wr("ACTIONDAG SOV waiting DAG_IDS spins=");
                wrint(sspins);
                wr("\r\n");
            }
            spin_delay();
            nsi(SYS_YIELD, 0, 0, 0);
        }
    }
    wr("ACTIONDAG SOV ids spins=");
    wrint(sspins);
    wr("\r\n");

    /* Arm 2: the child BEFORE its parent must be refused, and specifically with
     * -EAGAIN — the operator has the authority, the dependency is unmet. */
    long early = nsi(SYS_APPROVE_ACTION, (long)ids[1], 0, 0);
    if (early != -EAGAIN)
        fail("approving a child before its parent was not -EAGAIN", early);

    /* Arm 3: parent first, then the child succeeds. Without this, arm 2 is
     * satisfied by a queue that refuses everything. */
    long pa = nsi(SYS_APPROVE_ACTION, (long)ids[0], 0, 0);
    if (pa != 0)
        fail("approving the parent", pa);
    long ca = nsi(SYS_APPROVE_ACTION, (long)ids[1], 0, 0);
    if (ca != 0)
        fail("approving the child after its parent", ca);

    /* Arm 5: reject the second parent; its child must remain unapprovable, with
     * no cascade code in the kernel. */
    long rj = nsi(SYS_REJECT_ACTION, (long)ids[2], 0, 0);
    if (rj != 0)
        fail("rejecting the second parent", rj);
    long rc = nsi(SYS_APPROVE_ACTION, (long)ids[3], 0, 0);
    if (rc != -EAGAIN)
        fail("child of a REJECTED parent was approvable", rc);

    wr("PRADYOS_ACTIONDAG_OK\r\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
