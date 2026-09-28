/* user/actionspawntest.c — Section 3C ACTION_SPAWN_PROCESS (DDR-1017).
 *
 * The second force-pending type, built to DDR-1016's shape. `SPAWN_PROCESS` is
 * in `aether_action_forces_pending()` (DDR-842 S4), so it is never auto-approved
 * even in sovereign mode, and there is no approver in a gate boot: the verdict
 * must stay AE_PENDING and no process may appear on the action's behalf.
 *
 * THE EFFECT IS ASKED OF THE KERNEL, NOT ASSERTED BY THE PROBE. "I did not
 * fork" is the probe describing itself, which is worth nothing -- this repo has
 * now hit that failure mode five times. So the probe calls
 * `wait4(-1, &st, WNOHANG)` and reports the raw return code:
 *
 *   -ECHILD  (-10)  this process has no children at all      <- required
 *   -EAGAIN  (-11)  a child exists and has not exited yet    <- a fork happened
 *   > 0             a child exited and was reaped            <- a fork happened
 *
 * Both mutant outcomes are distinguishable from the required one, and the number
 * comes out of `sched_find_child` walking the real thread ring.
 *
 * THE CONTROL. `post=-10` means nothing unless fork+reap actually work in this
 * boot -- if fork were broken, a mutant that forked would also report no child
 * and the gate would measure nothing. So the probe first forks a child of its
 * own, with its own authority and no action involved, and reaps it: `ctrl=1`
 * requires BOTH the reaped pid and the exit status to match. Only then is
 * `post=-10` evidence. (`smoke-spawndepth` already proves the fork *cap*; it
 * does not prove fork works in THIS probe's boot, and a gate that leans on
 * another gate's boot is not measuring its own.)
 *
 * NOTE the rate limit, DDR-1016 §4: an agent gets AETHER_RATE_MAX = 60 syscalls
 * per 100 ticks and is KILLED past it, and a force-pending action never breaks a
 * poll loop early. Two polls around a ring-3 spin, as there.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#include "uline.h"          /* DDR-1056: one write per measured line */

#define SYS_WRITE          6
#define SYS_EXIT           4
#define SYS_FORK          15
#define SYS_WAIT4         16
#define SYS_SUBMIT_ACTION 31
#define SYS_POLL_RESULT   32

#define WNOHANG    1
#define NEG_ECHILD (-10)

/* Wire format, pinned by _Static_assert in aether.h in this same commit
 * (DDR-1013 §1: a probe constant drifted once with no gate able to see it). */
#define ACTION_SPAWN_PROCESS 3
#define AE_PENDING           1

#define CHILD_EXIT 7

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

static void wrdec(long v) {
    char b[24]; int i = 0;
    if (v < 0) { wr("-"); v = -v; }
    if (!v) { wr("0"); return; }
    while (v && i < 23) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) { char c[2]; c[0] = b[i]; c[1] = 0; wr(c); }
}

__attribute__((noreturn)) static void fail(const char *why, long v) {
    wr("ACTIONSPAWN FAIL: "); wr(why); wr(" rc="); wrdec(v); wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    static const char what[] = "/EXECTEST.ELF";

    /* 1. THE CONTROL. Our own fork, our own authority, no action.
     *
     * ctrl is COMPUTED, not asserted. An earlier draft called fail() on each
     * control mismatch and then printed a literal ctrl=1 -- which meant a broken
     * control could never reach the line, so the gate's `ctrl` check could never
     * fire and the number could not vary. That is the same dead-arm DDR-1016 §5
     * found in its `st` check, and it is worth naming as a class: a field whose
     * only possible value is the passing one is decoration, not measurement.
     * Now a broken control prints ctrl=0 and the gate says so. */
    int ctrl_ok = 0;
    long kid = nsi(SYS_FORK, 0, 0, 0);
    if (kid == 0) {
        nsi(SYS_EXIT, CHILD_EXIT, 0, 0);
        for (;;) { }
    }
    if (kid > 0) {
        int cst = -1;
        long reaped = nsi(SYS_WAIT4, kid, (long)&cst, 0);   /* blocking */
        ctrl_ok = (reaped == kid && cst == CHILD_EXIT);
    }

    /* 2. PROPOSE the spawn. The payload names what would be spawned. */
    long id = nsi(SYS_SUBMIT_ACTION, ACTION_SPAWN_PROCESS, (long)what, slen(what));
    if (id < 0)
        fail("submit", id);

    /* 3. Poll for a verdict that must never come. The first poll's value is the
     * reported one: aether_poll frees the slot on any terminal verdict, so an
     * unconditional second poll would return -ESRCH and the printed st could
     * only ever be 1 -- DDR-1016 §5 found exactly that arm dead. */
    long st = nsi(SYS_POLL_RESULT, id, 0, 0);
    if (st < 0)
        fail("poll1", st);
    if (st == AE_PENDING) {
        for (volatile long i = 0; i < 4000000L; i++) { }   /* 0 syscalls */
        st = nsi(SYS_POLL_RESULT, id, 0, 0);
        if (st < 0)
            fail("poll2", st);
    }

    /* 4. ASK THE KERNEL whether anything was spawned. The control child is
     * already reaped, so any child now is one the action produced. */
    int st2 = 0;
    long post = nsi(SYS_WAIT4, -1, (long)&st2, WNOHANG);

    { uline u; ul_init(&u);                       /* DDR-1056: ONE write */
      ul_s(&u, "PRADYOS_ACTIONSPAWN_OK id="); ul_d(&u, id);
      ul_s(&u, " st=");   ul_d(&u, st);
      ul_s(&u, " ctrl="); ul_d(&u, ctrl_ok);
      ul_s(&u, " post="); ul_d(&u, post);
      ul_s(&u, "\n"); wr(ul_end(&u)); }

    nsi(SYS_EXIT, (st == AE_PENDING && post == NEG_ECHILD) ? 0 : 1, 0, 0);
    for (;;) { }
}
