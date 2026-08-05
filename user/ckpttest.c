/* user/ckpttest.c — DDR-837 checkpoint/resume gate (NSI 84/85).
 *
 * ONE ELF, THREE ROLES, each self-identified at runtime rather than through an
 * argument channel:
 *
 *   controller (sovereign) — `SYS_CHECKPOINT_AGENT(1)` returns -EINVAL to it
 *                            (init may not be frozen). Drives arms 1-3.
 *   target     (plain)     — sees -EPERM, then finds its OWN pid in ps under the
 *                            name "CKPT_T". Loops: compute, then one SYS_YIELD.
 *   deny       (agent)     — sees -EPERM, does not find itself named "CKPT_T".
 *                            Runs arm 4.
 *
 * The controller locates the target BY NAME through SYS_GETPROCS, so no pid has
 * to be agreed in advance, and asserts on the target's observed `state` — never
 * on elapsed time, because a timing-based assertion on a scheduling feature is a
 * flake generator.
 *
 * The target must NOT spin on syscalls: agents are capped at 60 syscalls/s
 * (ADR-026 D7) and a syscall spin is killed at 137 before anything is observed.
 *
 * NO WRITABLE GLOBALS (DDR-826): one R+X PT_LOAD, everything on the stack.
 */
#include <stdint.h>

#define SYS_WRITE             6
#define SYS_EXIT              4
#define SYS_YIELD             3
#define SYS_GETPID            2
#define SYS_GETPROCS          67
#define SYS_CHECKPOINT_AGENT  84
#define SYS_RESUME_AGENT      85

#define EPERM   1
#define EINVAL  22
#define ESRCH   3

#define THREAD_BLOCKED 3

struct procinfo {
    uint32_t pid;
    uint32_t ppid;
    uint32_t state;
    uint32_t flags;
    char     name[16];
    uint64_t run_ticks;
    uint64_t dispatches;
};

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
    wr("CKPT FAIL: ");
    wr(why);
    wr(" rc=");
    wrint(rc);
    wr("\r\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

static int name_is(const char *n, const char *want) {
    int i = 0;
    for (; want[i]; i++)
        if (n[i] != want[i])
            return 0;
    return n[i] == 0;
}

/* Find the target by NAME and report its pid and state. Returns 1 if found. */
static int find_target(uint32_t *pid_out, uint32_t *state_out) {
    struct procinfo pi;
    for (long i = 0; i < 64; i++) {
        long r = nsi(SYS_GETPROCS, i, (long)&pi, 0);
        if (r <= 0)
            break;
        if (name_is(pi.name, "CKPT_T")) {
            *pid_out = pi.pid;
            *state_out = pi.state;
            return 1;
        }
    }
    return 0;
}

/* Pure computation, no syscalls: the target must stay under the agent syscall
 * rate cap, and the controller needs to yield without spinning on syscalls. */
static void spin_delay(void) {
    for (volatile long i = 0; i < 300000; i++) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    long who = nsi(SYS_CHECKPOINT_AGENT, 1, 0, 0);   /* init: -EINVAL vs -EPERM */

    if (who == -EPERM) {
        /* Two roles land here: the plain TARGET and the AGENT deny instance.
         * SYS_GETPROCS is available to both; the target identifies itself by
         * finding its OWN name. */
        struct procinfo self;
        long mypid = nsi(SYS_GETPID, 0, 0, 0);
        int am_target = 0;
        for (long i = 0; i < 64; i++) {
            long r = nsi(SYS_GETPROCS, i, (long)&self, 0);
            if (r <= 0) break;
            if ((long)self.pid == mypid && name_is(self.name, "CKPT_T")) {
                am_target = 1;
                break;
            }
        }

        if (am_target) {
            /* Loop forever: compute, then ONE yield. The controller freezes and
             * thaws this thread and observes its state through ps. */
            for (;;) {
                spin_delay();
                nsi(SYS_YIELD, 0, 0, 0);
            }
        }

        /* --- arm 4: agent, no sovereignty --- */
        long r = nsi(SYS_RESUME_AGENT, 2, 0, 0);
        if (r != -EPERM)
            fail("non-sovereign SYS_RESUME_AGENT was not denied", r);
        wr("PRADYOS_CKPT_DENY_OK\r\n");
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }

    /* --- controller (sovereign) --- */
    if (who != -EINVAL)
        fail("sovereign checkpoint of init was not -EINVAL", who);

    /* Arm 3: an unknown pid is -ESRCH, not -EPERM and not a silent success. */
    long ns = nsi(SYS_CHECKPOINT_AGENT, 60000, 0, 0);
    if (ns != -ESRCH)
        fail("checkpoint of an unknown pid was not -ESRCH", ns);

    /* Wait for the target to exist. Bounded: never spin forever in a gate. */
    uint32_t tpid = 0, tstate = 0;
    int found = 0;
    for (int i = 0; i < 200 && !found; i++) {
        found = find_target(&tpid, &tstate);
        if (!found) { spin_delay(); nsi(SYS_YIELD, 0, 0, 0); }
    }
    if (!found)
        fail("target CKPT_T never appeared in ps", 0);

    /* Arm 1: checkpoint, then wait for the target to reach its next syscall and
     * block ITSELF. Asserted on observed STATE, never on elapsed time. */
    long cp = nsi(SYS_CHECKPOINT_AGENT, (long)tpid, 0, 0);
    if (cp != 0)
        fail("checkpoint of the live target", cp);

    int blocked = 0;
    for (int i = 0; i < 400 && !blocked; i++) {
        if (find_target(&tpid, &tstate) && tstate == THREAD_BLOCKED)
            blocked = 1;
        else { spin_delay(); nsi(SYS_YIELD, 0, 0, 0); }
    }
    if (!blocked)
        fail("target never reached THREAD_BLOCKED after checkpoint", (long)tstate);

    /* Arm 2: resume, then wait for it to leave BLOCKED. */
    long rs = nsi(SYS_RESUME_AGENT, (long)tpid, 0, 0);
    if (rs != 0)
        fail("resume of the checkpointed target", rs);

    int running = 0;
    for (int i = 0; i < 400 && !running; i++) {
        if (find_target(&tpid, &tstate) && tstate != THREAD_BLOCKED)
            running = 1;
        else { spin_delay(); nsi(SYS_YIELD, 0, 0, 0); }
    }
    if (!running)
        fail("target stayed BLOCKED after resume", (long)tstate);

    wr("PRADYOS_CKPT_OK\r\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
