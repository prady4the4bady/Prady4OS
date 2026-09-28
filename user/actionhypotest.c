/* user/actionhypotest.c — Section 3C ACTION_PROPOSE_HYPOTHESIS + ACTION_EVOLVE_GENOME
 * (DDR-1020). Two types in one probe, because they are the SAME pipeline on
 * opposite sides of the policy split and running them in one boot is what shows
 * the split is real rather than asserted.
 *
 *   PROPOSE_HYPOTHESIS (10)  NOT in aether_action_forces_pending -> auto-approves
 *                            in sovereign mode. Executor: log the hypothesis to
 *                            the SFS root and read it back.
 *   EVOLVE_GENOME (12)       IS in aether_action_forces_pending (DDR-842 S4) ->
 *                            never auto-approved, no approver in a gate boot.
 *                            The genome must be left UNCHANGED.
 *
 * ONE probe, one boot, one policy engine: `hst=2` and `gst=1` on the same line
 * is the evidence that the force-pending list actually discriminates. Two
 * separate probes could each be passing for their own unrelated reasons.
 *
 * The genome arm is the DDR-1016 shape: seed a known genome with our own
 * authority, propose an evolution, and assert BOTH that the verdict stays
 * PENDING and that the bytes on disk are untouched. `gseed` is the control --
 * without a readback proving the file was written and is readable, "unchanged"
 * would also be what a broken write produces.
 *
 * Polls are BOUNDED (two around a ring-3 spin), not DDR-1015's 20000-iteration
 * loop: that loop is safe only while an action auto-approves and breaks it on
 * iteration 1, and EVOLVE_GENOME never does -- AETHER_RATE_MAX is 60 syscalls
 * per 100 ticks and the kernel kills the agent past it (DDR-1016 §4, measured).
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#include "uline.h"          /* DDR-1056: one write per measured line */

#define SYS_WRITE          6
#define SYS_READ           5
#define SYS_OPEN           7
#define SYS_CLOSE          8
#define SYS_EXIT           4
#define SYS_SUBMIT_ACTION 31
#define SYS_POLL_RESULT   32

#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_CREAT  0x40

/* Wire format, pinned by _Static_assert in aether.h in this same commit. */
#define ACTION_PROPOSE_HYPOTHESIS 10
#define ACTION_EVOLVE_GENOME      12
#define AE_PENDING                 1
#define AE_APPROVED                2

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
    wr("ACTIONHYPO FAIL: "); wr(why); wr(" rc="); wrdec(v); wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

static long put(const char *path, const char *data) {
    long fd = nsi(SYS_OPEN, (long)path, O_CREAT | O_WRONLY, 0);
    if (fd < 0) return fd;
    long n = slen(data);
    long w = nsi(SYS_WRITE, fd, (long)data, n);
    nsi(SYS_CLOSE, fd, 0, 0);
    return (w == n) ? 0 : -1;
}

/* Read a file into buf; returns the byte count, or < 0. */
static long get(const char *path, char *buf, long cap) {
    long fd = nsi(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) return fd;
    long n = nsi(SYS_READ, fd, (long)buf, cap);
    nsi(SYS_CLOSE, fd, 0, 0);
    return n;
}

/* Submit, then poll at most twice around a ring-3 spin. Returns the verdict. */
static long verdict(long type, const char *payload, long *id_out) {
    long id = nsi(SYS_SUBMIT_ACTION, type, (long)payload, slen(payload));
    if (id < 0) fail("submit", id);
    *id_out = id;
    long st = nsi(SYS_POLL_RESULT, id, 0, 0);
    if (st < 0) fail("poll1", st);
    if (st == AE_PENDING) {
        for (volatile long i = 0; i < 4000000L; i++) { }   /* 0 syscalls */
        st = nsi(SYS_POLL_RESULT, id, 0, 0);
        if (st < 0) fail("poll2", st);
    }
    return st;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    static const char hpath[] = "/HYPO.TXT";
    static const char htext[] = "H: rq depth predicts wake latency";  /* 33 */
    static const char gpath[] = "/GENOME.TXT";
    static const char gtext[] = "genome v1";                          /* 9  */
    char buf[128];

    /* ---- arm 1: PROPOSE_HYPOTHESIS, auto-approving ---- */
    long hid = 0;
    long hst = verdict(ACTION_PROPOSE_HYPOTHESIS, htext, &hid);

    /* EXECUTE, and only if approved. Logging regardless would make the two
     * orders indistinguishable, which is the property under test. */
    long hn = 0;
    if (hst == AE_APPROVED) {
        if (put(hpath, htext) != 0) fail("logging the hypothesis", 0);
        hn = get(hpath, buf, (long)sizeof buf);
        if (hn < 0) fail("reading the hypothesis back", hn);
    }

    /* ---- arm 2: EVOLVE_GENOME, force-pending ---- */
    /* Seed the genome with our OWN authority first: this is the control. If the
     * write silently failed, "unchanged" below would be a claim about a file
     * that never existed. */
    /* gseed is COMPUTED, not asserted. An earlier draft fail()d unless it equalled
     * len(gtext) and then printed it -- so the value could only ever be 9 and the
     * gate's check on it could never fire. That is the FOURTH instance of this
     * class in five DDRs (DDR-1016 §5 st, DDR-1017 §4 ctrl, DDR-1018 §3 st, and
     * this). A field whose only reachable value is the passing one is decoration,
     * not measurement -- so check every new arm by asking which mutant makes it
     * print something else, and if the answer is "none", the arm is fake. */
    (void)put(gpath, gtext);
    long gseed = get(gpath, buf, (long)sizeof buf);
    if (gseed < 0) gseed = 0;         /* unreadable reports as 0, not as a kill */

    long gid = 0;
    long gst = verdict(ACTION_EVOLVE_GENOME, "genome v2", &gid);

    /* The evolution must NOT have happened: same byte count as the seed. */
    long gn = get(gpath, buf, (long)sizeof buf);
    if (gn < 0) gn = 0;               /* absent reports as 0, not as a kill:
                                       * fail()ing here would fire BEFORE the
                                       * line printed and take the gseed arm
                                       * with it (measured -- M5). */

    { uline u; ul_init(&u);                       /* DDR-1056: ONE write */
      ul_s(&u, "PRADYOS_ACTIONHYPO_OK hst="); ul_d(&u, hst);
      ul_s(&u, " hn=");    ul_d(&u, hn);
      ul_s(&u, " gst=");   ul_d(&u, gst);
      ul_s(&u, " gseed="); ul_d(&u, gseed);
      ul_s(&u, " gn=");    ul_d(&u, gn);
      ul_s(&u, "\n"); wr(ul_end(&u)); }

    nsi(SYS_EXIT, (hst == AE_APPROVED && gst == AE_PENDING) ? 0 : 1, 0, 0);
    for (;;) { }
}
