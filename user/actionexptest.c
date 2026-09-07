/* user/actionexptest.c — Section 3C ACTION_RUN_EXPERIMENT, end to end (DDR-1083).
 *
 * DDR-1021 assessed this type as "not buildable at any ring" on three facts.
 * DDR-1034 retired all three by building the executor and wiring CAP_EXEC, and
 * did not come back to the action type; DDR-1072 §2 recorded that `smoke-runexp`
 * gates the EXECUTOR while its name matches the TYPE. This probe is the caller
 * that closes the gap, and it is what makes aether.h's `ACTION_RUN_EXPERIMENT ==
 * 11` pin true: before this file, nothing hand-copied 11 (DDR-1083 §2).
 *
 * WHAT THIS PROVES, AND WHAT IT DOES NOT. `sys_run_experiment` checks is_exec and
 * cap_authorize and does NOT consult the action queue -- an agent holding the
 * door can call NSI 100 without submitting anything, before and after this
 * commit. So the claim is the propose -> arbitrate -> OBEY loop and its audit
 * record, not a new enforcement. That is the system's design for every action
 * type it has (DDR-1013 §2: the kernel is the policy engine, the AGENT acts).
 *
 * THE OBVIOUS ARM IS VACUOUS AND THAT WAS MEASURED BEFORE THIS WAS WRITTEN:
 * "submit, then assert the experiment computed 42" passes on a build with no
 * submit at all, because smoke-runexp ALREADY requires `PRADYOS_EXP_CALC rc=0
 * v=42` from user/exptest.c. What only the verdict path can produce is the
 * DECLINE -- hence arm B, which is the load-bearing half.
 *
 * ONE probe, ONE boot, both sides of one policy engine (DDR-1020's shape): two
 * separate probes could each pass for their own unrelated reasons.
 *
 *   arm A  RUN_EXPERIMENT, no parent    -> auto-approves (sovereign, and the type
 *                                          is not in aether_action_forces_pending)
 *                                          -> the probe RUNS it, v=42.
 *   arm B  DELETE_FILE (force-pending, so PENDING forever in a gate boot; no
 *          approver exists) as the PARENT of a RUN_EXPERIMENT child -> DDR-839's
 *          DAG holds the child PENDING -> the probe DECLINES, ran=0.
 *
 * Arm B's program computes a DIFFERENT value (97) from arm A's 42, so a spurious
 * run is visible in the value stream and not only in the flag; `v=97` is a
 * FORBIDDEN_SENTINEL on the gate. That costs nothing: smoke-runexp already
 * declares one, so per DDR-1043 it is already never early-exit eligible.
 *
 * The parent's TYPE is immaterial -- the only load-bearing property is
 * membership in aether_action_forces_pending(). Nothing in the kernel executes
 * DELETE_FILE (DDR-1013 §2) and this probe never acts on it, so it stays a queue
 * entry and an audit record. Rejected alternative: a bogus parent id, one
 * syscall cheaper and equally deterministic, but it exercises the "parent
 * missing" branch instead of the realistic "parent not yet approved" one that
 * DDR-839's DAG is actually about.
 *
 * POLLS ARE BOUNDED. This process holds is_agent, so AETHER_RATE_MAX (60
 * syscalls / 100 ticks) applies and an unbounded poll on a PENDING action is
 * killed with AGENT_RATE_LIMITED before anything prints -- the defect DDR-1016
 * §4 hit and measured. Total here is ~12 syscalls.
 *
 * The probe REPORTS and the gate JUDGES (DDR-1020): every field is printed
 * unconditionally, and no fail() precedes a print, so no arm can be silently
 * removed by an early exit.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld gives a
 * single R+X PT_LOAD, and ci-probe-rodata-check rejects any writable allocated
 * section -- the defect DDR-1032 hit with a static pointer array).
 */

#include "uline.h"          /* DDR-1056: one write(2) per measured line */

#define SYS_EXIT                 4
#define SYS_WRITE                6
#define SYS_SUBMIT_ACTION       31
#define SYS_POLL_RESULT         32
#define SYS_SUBMIT_CHILD_ACTION 92
#define SYS_RUN_EXPERIMENT     100

/* Action wire format, pinned by _Static_assert in kernel/aether/aether.h. This
 * file is the probe that pin's comment claims exists (DDR-1083 §2). */
#define ACTION_DELETE_FILE      6
#define ACTION_RUN_EXPERIMENT  11
#define AE_PENDING              1
#define AE_APPROVED             2

/* exp_op wire format, hand-copied from kernel/aether/experiment.h. */
#define OP_HALT 0
#define OP_PUSH 1
#define OP_SUB  3
#define OP_MUL  4

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* Four arguments: SYS_SUBMIT_CHILD_ACTION takes the parent id in a4 (DDR-839
 * made it a SEPARATE call rather than a fourth argument to NSI 31, because every
 * existing caller of 31 leaves that register undefined). */
static inline long nsi4(long n, long a1, long a2, long a3, long a4) {
    long r;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn)) static void fail(const char *why, long v) {
    uline u; ul_init(&u);
    ul_s(&u, "ACTIONEXP FAIL: "); ul_s(&u, why);
    ul_s(&u, " rc=");             ul_d(&u, v);
    ul_s(&u, "\n");
    wr(ul_end(&u));
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

/* PUSH + int64 little-endian immediate. */
static unsigned emit_push(unsigned char *c, unsigned p, long long v) {
    unsigned long u = (unsigned long)v;
    c[p++] = OP_PUSH;
    for (int i = 0; i < 8; i++) { c[p++] = (unsigned char)(u & 0xFF); u >>= 8; }
    return p;
}

/* Poll at most twice around a ring-3 spin (0 syscalls), then report whatever the
 * verdict is. A PENDING action never becomes anything else in a gate boot, so
 * this must not loop: see the rate-limit note in the header. */
static long settle(long id) {
    long st = nsi(SYS_POLL_RESULT, id, 0, 0);
    if (st < 0) fail("poll1", st);
    if (st == AE_PENDING) {
        for (volatile long i = 0; i < 4000000L; i++) { }
        st = nsi(SYS_POLL_RESULT, id, 0, 0);
        if (st < 0) fail("poll2", st);
    }
    return st;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    unsigned char code[64];
    unsigned p;
    long long v;
    long rc, id, st, ran;

    /* ---- arm A: propose, be approved, and only then run --------------------
     * 6 * 7 = 42. */
    p = 0;
    p = emit_push(code, p, 6);
    p = emit_push(code, p, 7);
    code[p++] = OP_MUL;
    code[p++] = OP_HALT;

    /* The program IS the payload: what the agent proposes is what it then runs,
     * so the queue entry and the audit record name the actual bytes. */
    id = nsi(SYS_SUBMIT_ACTION, ACTION_RUN_EXPERIMENT, (long)code, (long)p);
    if (id < 0) fail("submitA", id);
    st = settle(id);

    v = 0; rc = 0; ran = 0;
    if (st == AE_APPROVED) {
        rc = nsi(SYS_RUN_EXPERIMENT, (long)code, (long)p, (long)&v);
        ran = 1;
    }
    { uline u; ul_init(&u);
      ul_s(&u, "PRADYOS_EXPACT_A st="); ul_d(&u, st);
      ul_s(&u, " ran=");                ul_d(&u, ran);
      ul_s(&u, " rc=");                 ul_d(&u, rc);
      ul_s(&u, " v=");                  ul_d(&u, (long)v);
      ul_s(&u, "\n"); wr(ul_end(&u)); }

    /* ---- arm B: a verdict that is NOT approval, and obedience to it ---------
     * The parent is force-pending, so DDR-839's DAG holds the child PENDING.
     * 10 * 10 - 3 = 97, deliberately not 42 (see the header). */
    long pid_parent = nsi(SYS_SUBMIT_ACTION, ACTION_DELETE_FILE,
                          (long)"/NOSUCH.TXT", slen("/NOSUCH.TXT"));
    if (pid_parent < 0) fail("submitP", pid_parent);
    long pst = nsi(SYS_POLL_RESULT, pid_parent, 0, 0);
    if (pst < 0) fail("pollP", pst);

    p = 0;
    p = emit_push(code, p, 10);
    p = emit_push(code, p, 10);
    code[p++] = OP_MUL;
    p = emit_push(code, p, 3);
    code[p++] = OP_SUB;
    code[p++] = OP_HALT;

    id = nsi4(SYS_SUBMIT_CHILD_ACTION, ACTION_RUN_EXPERIMENT,
              (long)code, (long)p, pid_parent);
    if (id < 0) fail("submitB", id);
    st = settle(id);

    v = 0; rc = 0; ran = 0;
    if (st == AE_APPROVED) {
        rc = nsi(SYS_RUN_EXPERIMENT, (long)code, (long)p, (long)&v);
        ran = 1;
    }
    { uline u; ul_init(&u);
      ul_s(&u, "PRADYOS_EXPACT_B st="); ul_d(&u, st);
      ul_s(&u, " pst=");                ul_d(&u, pst);
      ul_s(&u, " ran=");                ul_d(&u, ran);
      ul_s(&u, " rc=");                 ul_d(&u, rc);
      ul_s(&u, " v=");                  ul_d(&u, (long)v);
      ul_s(&u, "\n"); wr(ul_end(&u)); }

    wr("PRADYOS_EXPACT_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
