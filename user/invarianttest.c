/* user/invarianttest.c — DDR-844 security invariant gate (S1–S8).
 *
 * EVERY OTHER GATE ASSERTS A FEATURE WORKS. THIS ONE ASSERTS ATTACKS FAIL.
 *
 * That inversion is the point. S1-S8 have been prose since Layer 6, and prose
 * cannot regress visibly: if a refactor deletes a capability check, every
 * feature still works and no existing gate notices — only the REFUSALS stop
 * happening. Each arm below performs a real attack from ring 3 and requires the
 * kernel to reject it.
 *
 * Runs as an AGENT deliberately. A sovereign probe would be permitted to do most
 * of this, and the gate would assert nothing.
 *
 * NO WRITABLE GLOBALS (DDR-826): one R+X PT_LOAD, everything on the stack.
 */
#include <stdint.h>

#define SYS_WRITE                 6
#define SYS_EXIT                  4
#define SYS_GETPID                2
#define SYS_FORK                 15
#define SYS_SUBMIT_ACTION        31
#define SYS_APPROVE_ACTION       33
#define SYS_SET_MEM_LIMIT        38
#define SYS_MEMORY_WRITE         82
#define SYS_APPROVE_CODE_REWRITE 86
#define SYS_VERIFY_AUDIT         93

#define ACTION_PRINT              2
#define ACTION_REWRITE_AGENT_CODE 9

#define EPERM   1
#define EFAULT 14
#define EINVAL 22
#define EAGAIN 11

#define AETHER_PAYLOAD_MAX 512
#define MEM_MAX_VAL        256

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
static void fail(const char *inv, const char *why, long rc) {
    wr("INVARIANT FAIL ["); wr(inv); wr("]: "); wr(why);
    wr(" rc="); wrint(rc); wr("\r\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}
static void ok(const char *inv) { wr("  [inv] "); wr(inv); wr(" upheld\r\n"); }

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    long mypid = nsi(SYS_GETPID, 0, 0, 0);

    /* ---- S1: no self-escalation ---- */
    /* Raise MY OWN memory cap. SYS_SET_MEM_LIMIT is lower-only for a reason; an
     * agent that can raise its own ceiling has escaped aether_mem entirely. */
    long esc = nsi(SYS_SET_MEM_LIMIT, mypid, (long)(1ull << 40), 0);
    if (esc == 0)
        fail("S1", "agent raised its OWN mem_limit", esc);
    ok("S1 self mem_limit raise refused");

    /* Approve my own action. The whole human-gate model collapses if an agent
     * can approve what it submitted. */
    long mine = nsi(SYS_SUBMIT_ACTION, ACTION_PRINT, (long)"self", 4);
    if (mine >= 0) {
        long self_ok = nsi(SYS_APPROVE_ACTION, mine, 0, 0);
        if (self_ok == 0)
            fail("S1", "agent approved its OWN action", self_ok);
        ok("S1 self-approval refused");
    }

    /* ---- S2: bounded everything, and never a panic ---- */
    /* Oversized action payload. Must clamp or refuse; the kernel surviving to
     * print the next line IS the assertion. */
    char big[AETHER_PAYLOAD_MAX * 2];
    for (int i = 0; i < AETHER_PAYLOAD_MAX * 2; i++) big[i] = 'A';
    (void)nsi(SYS_SUBMIT_ACTION, ACTION_PRINT, (long)big, AETHER_PAYLOAD_MAX * 2);
    ok("S2 oversized payload survived (clamped or refused, no panic)");

    /* Oversized agent-memory value. */
    long omem = nsi(SYS_MEMORY_WRITE, (long)"S2KEY", (long)big, MEM_MAX_VAL * 2);
    if (omem == 0)
        fail("S2", "oversized memory write ACCEPTED", omem);
    ok("S2 oversized memory write refused");

    /* ---- S6: fault isolation — bad user pointers must not take the kernel ---- */
    /* A kernel-half pointer handed to a user-pointer argument. */
    long kptr = nsi(SYS_MEMORY_WRITE, (long)0xFFFFFFFF80000000ull, (long)big, 8);
    if (kptr == 0)
        fail("S6", "kernel pointer accepted as a user pointer", kptr);
    ok("S6 kernel pointer rejected");

    /* A non-canonical address — the classic #GP source if unchecked. */
    long nc = nsi(SYS_MEMORY_WRITE, (long)0x0000800000000000ull, (long)big, 8);
    if (nc == 0)
        fail("S6", "non-canonical pointer accepted", nc);
    ok("S6 non-canonical pointer rejected");

    /* ---- S4 / S8: the human gate is structural ---- */
    /* An agent must not be able to approve a code rewrite, with or without
     * sovereign mode being on. */
    long crw = nsi(SYS_APPROVE_CODE_REWRITE, 1, 0, 0);
    if (crw == 0)
        fail("S4/S8", "agent approved a code rewrite", crw);
    ok("S4/S8 agent code-rewrite approval refused");

    /* An agent must not be able to read the audit verdict either — that is an
     * operator question (S5's read side is CAP_SOVEREIGN). */
    long va = nsi(SYS_VERIFY_AUDIT, 0, 0, 0);
    if (va != -EPERM)
        fail("S5", "agent could query the audit chain verdict", va);
    ok("S5 agent audit query refused");

    /* ---- S2: spawn depth, asserted here as an INVARIANT ----
     * smoke-spawndepth gates it as a feature; an invariant gate must not assume
     * another gate ran. Four forks from an agent must hit the cap. */
    int depth = 0;
    for (;;) {
        long f = nsi(SYS_FORK, 0, 0, 0);
        if (f < 0) {
            if (f != -EAGAIN)
                fail("S2", "fork refused with the wrong errno", f);
            break;
        }
        if (f > 0) { nsi(SYS_EXIT, 0, 0, 0); for (;;) { } }   /* parent leaves */
        if (++depth > 3)
            fail("S2", "forked PAST the spawn-depth cap", depth);
    }
    ok("S2 spawn depth capped");

    wr("PRADYOS_INVARIANTS_OK S1,S2,S4,S5,S6,S8\r\n");
    /* S3 and S7 are NOT claimed: they depend on F#66-72 (immutable objective
     * function, independent verifier), which are not built. A green arm for an
     * unbuilt subsystem would convert "not implemented" into "verified". The
     * sentinel enumerates what was actually covered. */
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
