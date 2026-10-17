/* user/actiondeltest.c — Section 3C ACTION_DELETE_FILE, end to end (DDR-1016).
 *
 * This is the SECOND 3C type and the FIRST force-pending one, so it is a
 * different shape from DDR-1015's ACTION_READ_FILE probe, not a copy of it.
 *
 * ACTION_DELETE_FILE is in aether_action_forces_pending() (aether.h) alongside
 * SPAWN_PROCESS / REWRITE_AGENT_CODE / EVOLVE_GENOME. DDR-842 S4 names each as
 * needing a human gate, so it is NEVER auto-approved -- not even in sovereign
 * mode, which is the ADR-026 D2 default this kernel boots in. There is no
 * approver in this boot. So the correct assertion is the opposite of
 * DDR-1015's: the verdict must STAY PENDING, and the file must SURVIVE.
 *
 * That second half is the point, and it is also the arm DDR-1015 §5 recorded as
 * UNMEASURED. §5 observed that a probe which acted first and asked afterwards
 * would print an identical line -- true for READ_FILE, because a read leaves no
 * trace. A DELETE does. Here the two orders are distinguishable in the
 * filesystem itself: act-then-ask leaves the target gone. That is what makes
 * this the natural place to measure the ordering, exactly as §5 predicted.
 *
 * THE CONTROL. "The target still exists" is only evidence if unlink actually
 * works on this root in this boot -- otherwise a broken SYS_UNLINK would make
 * the probe pass while measuring nothing, and the M1 mutant below would pass
 * too. So the probe deletes a SECOND file, /ADELCTRL, with its own authority
 * and no action at all, through the same call the mutant would use, and
 * verifies it is gone. ctrl=1 is what licenses reading keep=1 as a result.
 *
 *   1. create /ADELKEEP and /ADELCTRL on the SFS root
 *   2. unlink /ADELCTRL outright  -> gone      (the control: unlink works)
 *   3. submit ACTION_DELETE_FILE("/ADELKEEP")  -> action_id
 *   4. poll twice, separated by real elapsed time -> AE_PENDING both times
 *   5. open /ADELKEEP                          -> still there (never acted on)
 *
 * A FORCE-PENDING PROBE MUST NOT BUSY-POLL, and this is a structural difference
 * from DDR-1015's probe, not a style choice. aether_check_rate (aether_mem.c:59)
 * kills any agent that exceeds AETHER_RATE_MAX = 60 syscalls in a 100-tick
 * window. DDR-1015's actionreadtest.c loops up to 20000 times and is safe only
 * because an auto-approved action breaks it on the FIRST iteration. A
 * force-pending action never breaks it -- the first draft of this file used the
 * same shape and the kernel killed the agent with AGENT_RATE_LIMITED PID=37,
 * measured, before any line was printed. The other three force-pending types
 * (SPAWN_PROCESS / REWRITE_AGENT_CODE / EVOLVE_GENOME) will hit exactly this.
 *
 * So the wait between the two polls is spent in a RING-3 SPIN, which costs zero
 * syscalls. The thread is still preemptible there -- the timer interrupt does
 * not care that the loop makes no calls -- so real time passes and the sliding
 * rate window drains, for the price of one poll instead of hundreds.
 *
 * Both polls land far under AETHER_ACTION_TTL_TICKS (6000 ticks = 60 s), so
 * AE_EXPIRED is not a confound; and if it ever became one the gate reports
 * st=4 by number rather than folding it into "not approved".
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#include "uline.h"          /* DDR-1056: one write per measured line */

#define SYS_WRITE          6
#define SYS_READ           5
#define SYS_OPEN           7
#define SYS_CLOSE          8
#define SYS_EXIT           4
#define SYS_YIELD          3
#define SYS_UNLINK        68
#define SYS_SUBMIT_ACTION 31
#define SYS_POLL_RESULT   32

#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_CREAT  0x40

/* Wire format. aether.h pins this with _Static_assert in the same commit as
 * this file, for the DDR-1013 §1 reason: actiondagtest.c had drifted to a wrong
 * constant and no gate could see it. If the enum shifts, the KERNEL stops
 * building. */
#define ACTION_DELETE_FILE 6
#define AE_PENDING         1

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
    wr("ACTIONDEL FAIL: "); wr(why); wr(" rc="); wrdec(v); wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

/* create + write a few bytes, so the file has real content and a real extent. */
static void make(const char *path) {
    static const char payload[] = "actiondel payload";
    long fd = nsi(SYS_OPEN, (long)path, O_CREAT | O_WRONLY, 0);
    if (fd < 0) fail("create", fd);
    long n = slen(payload);
    if (nsi(SYS_WRITE, fd, (long)payload, n) != n) fail("write short", n);
    nsi(SYS_CLOSE, fd, 0, 0);
}

/* 1 if the path opens for reading, 0 if it does not. */
static int present(const char *path) {
    long fd = nsi(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) return 0;
    nsi(SYS_CLOSE, fd, 0, 0);
    return 1;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    static const char keep[] = "/ADELKEEP";
    static const char ctrl[] = "/ADELCTRL";

    /* 1. Both files exist to begin with. Asserted, not assumed: if the create
     * silently failed, "the target survived" would be a lie about a file that
     * was never there. */
    make(keep);
    make(ctrl);
    if (!present(keep)) fail("keep absent after create", 0);
    if (!present(ctrl)) fail("ctrl absent after create", 0);

    /* 2. THE CONTROL. No action, no verdict -- just the probe's own authority
     * over its own file, through the same SYS_UNLINK an approved delete would
     * eventually use. This is what proves the deletion path works here. */
    long urc = nsi(SYS_UNLINK, (long)ctrl, 0, 0);
    if (urc < 0) fail("control unlink", urc);
    int ctrl_gone = !present(ctrl);

    /* 3. PROPOSE the delete. The payload is the path the action concerns. */
    long id = nsi(SYS_SUBMIT_ACTION, ACTION_DELETE_FILE, (long)keep, slen(keep));
    if (id < 0) fail("submit", id);

    /* 4. Poll for a verdict that must never come. Twice, not in a loop.
     *
     * Auto-approval -- the failure this arm exists to catch -- happens
     * synchronously inside aether_submit, so the first poll already sees it.
     * The second, after real elapsed time, shows the verdict does not arrive
     * LATER either. Two is the whole useful count.
     *
     * THE FIRST POLL'S VALUE IS THE ONE REPORTED, and the second is skipped
     * unless the first says PENDING. That is forced by aether_poll: a terminal
     * verdict (APPROVED / REJECTED / EXPIRED) is latched once and the slot is
     * FREED, so polling again returns -ESRCH. A probe that always polled twice
     * would therefore fail with rc=-3 on any non-pending verdict and the st it
     * printed could only ever be 1 -- a number that looks like a measurement
     * and cannot vary. Measured: the M2 mutant (DELETE_FILE removed from
     * aether_action_forces_pending) failed exactly that way before this branch
     * existed, reporting a vanished slot instead of the auto-approval that
     * caused it. Now it reports st=2 and the gate names the defect. */
    long st = nsi(SYS_POLL_RESULT, id, 0, 0);
    if (st < 0) fail("poll1", st);

    if (st == AE_PENDING) {
        /* Let real time pass without spending one syscall (see the header). */
        for (volatile long i = 0; i < 4000000L; i++) { }
        st = nsi(SYS_POLL_RESULT, id, 0, 0);
        if (st < 0) fail("poll2", st);
    }

    /* 5. THE EFFECT MUST NOT HAVE HAPPENED. */
    int keep_present = present(keep);

    /* DDR-1056: ONE write. The gate greps this line WHOLE, and nine writes
     * are nine console-lock acquisitions with eight gaps for another CPU. */
    { uline u; ul_init(&u);
      ul_s(&u, "PRADYOS_ACTIONDEL_OK id="); ul_d(&u, id);
      ul_s(&u, " st=");   ul_d(&u, st);
      ul_s(&u, " ctrl="); ul_d(&u, ctrl_gone);
      ul_s(&u, " keep="); ul_d(&u, keep_present);
      ul_s(&u, "\n"); wr(ul_end(&u)); }

    /* Tidy up: leave the SFS root as we found it, so a later boot of the same
     * image does not inherit /ADELKEEP. The gate has already been given its
     * numbers on the line above; this cannot affect them. */
    nsi(SYS_UNLINK, (long)keep, 0, 0);

    nsi(SYS_EXIT, st == AE_PENDING ? 0 : 1, 0, 0);
    for (;;) { }
}
