/* user/killagenttest.c — DDR-1159 SYS_KILL_AGENT gate (NSI 36, Fix 3).
 *
 * NSI 36 (the sovereign/agent kill switch) had NO probe or gate — this is the
 * first. It covers the rerouted sys_kill_agent write path (now
 * sched_signal_user_pid, find+set sig_pending under g_sched_lock) FUNCTIONALLY.
 * It does NOT reproduce the SMP use-after-free the fix closes — that race needs
 * a second CPU freeing the target in the find->deref window and cannot be driven
 * single-CPU (DDR-1159 §6).
 *
 * ONE ELF, TWO ROLES, self-identified by SYS_KILL_AGENT's return code on an
 * UNKNOWN pid (never pid 1 — SYS_KILL_AGENT has no pid-1/self guard, so probing
 * init would try to kill it):
 *
 *   controller (sovereign): SYS_KILL_AGENT(60000) -> -ESRCH (authorized, pid
 *                           absent). That IS arm A1. Drives A2.
 *   target KA_T (plain):    SYS_KILL_AGENT(60000) -> -EPERM (no CAP_AGENT).
 *                           Asserts the denial, prints DENY_OK, then spins
 *                           (compute + one yield) so the controller can find it
 *                           alive by name and kill it.
 *
 * The controller asserts on the target's observed STATE (gone, or ZOMBIE/DONE),
 * never on elapsed time — a timing assertion on a kill is a flake generator.
 *
 * NO WRITABLE GLOBALS (DDR-826): one R+X PT_LOAD, everything on the stack.
 */
#include <stdint.h>

#define SYS_WRITE        6
#define SYS_EXIT         4
#define SYS_YIELD        3
#define SYS_GETPROCS     67
#define SYS_KILL_AGENT   36

#define EPERM   1
#define ESRCH   3

#define THREAD_DONE    2
#define THREAD_ZOMBIE  4

#define UNKNOWN_PID  60000

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

__attribute__((noreturn)) static void fail(const char *why, long rc) {
    wr("KILLAGENT FAIL: ");
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

/* Find KA_T by NAME; report pid + state. Returns 1 if present in ps. */
static int find_target(uint32_t *pid_out, uint32_t *state_out) {
    struct procinfo pi;
    for (long i = 0; i < 64; i++) {
        long r = nsi(SYS_GETPROCS, i, (long)&pi, 0);
        if (r <= 0)
            break;
        if (name_is(pi.name, "KA_T")) {
            *pid_out = pi.pid;
            *state_out = pi.state;
            return 1;
        }
    }
    return 0;
}

/* Pure computation, no syscalls — just a scheduling delay between polls. */
static void spin_delay(void) {
    for (volatile long i = 0; i < 300000; i++) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    long who = nsi(SYS_KILL_AGENT, UNKNOWN_PID, 0, 0);

    if (who == -EPERM) {
        /* --- target KA_T (plain user): denial proven, then be killable. --- */
        wr("PRADYOS_KILLAGENT_DENY_OK\r\n");
        for (;;) {
            spin_delay();
            nsi(SYS_YIELD, 0, 0, 0);   /* the kill lands on the IRQ return / yield */
        }
    }

    /* --- controller (sovereign). A1: an unknown pid is -ESRCH, not a silent
     * success and not -EPERM. --- */
    if (who != -ESRCH)
        fail("authorized kill of an unknown pid was not -ESRCH", who);

    /* Wait for KA_T to exist. Bounded: never spin forever in a gate. */
    uint32_t tpid = 0, tstate = 0;
    int found = 0;
    for (int i = 0; i < 200 && !found; i++) {
        found = find_target(&tpid, &tstate);
        if (!found) { spin_delay(); nsi(SYS_YIELD, 0, 0, 0); }
    }
    if (!found)
        fail("target KA_T never appeared in ps", 0);

    /* A2: kill the LIVE target -> 0, then wait for it to actually die. The
     * return value alone is not the proof (a no-op write would also return 0);
     * the proof is the target leaving the ring / reaching ZOMBIE/DONE. */
    long k = nsi(SYS_KILL_AGENT, (long)tpid, 0, 0);
    if (k != 0)
        fail("kill of the live target did not return 0", k);

    int dead = 0;
    for (int i = 0; i < 600 && !dead; i++) {
        uint32_t p = 0, s = 0;
        if (find_target(&p, &s)) {
            if (s == THREAD_ZOMBIE || s == THREAD_DONE)
                dead = 1;
        } else if (nsi(SYS_KILL_AGENT, (long)tpid, 0, 0) == -ESRCH) {
            /* A missed scan is NOT proof of death: SYS_GETPROCS is a best-effort
             * indexed snapshot, so a live KA_T can slip behind the scan index.
             * Confirm with the kernel: -ESRCH means no live user thread has
             * this pid. A live one would return 0 (a harmless re-signal). */
            dead = 1;
        }
        if (!dead) {
            spin_delay(); nsi(SYS_YIELD, 0, 0, 0);
        }
    }
    if (!dead)
        fail("target still alive after kill", (long)tstate);

    wr("PRADYOS_KILLAGENT_OK\r\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
