/* user/actionreadtest.c — Section 3C ACTION_READ_FILE, end to end (DDR-1015).
 *
 * DDR-1013 §2.1 specified what "implement a 3C action type" means, and it is
 * RING-3 work: the kernel is the policy engine, the agent is the executor. There
 * is no kernel-side executor for ACTION_READ_FILE, and there is none for
 * ACTION_WRITE_FILE either -- that is the architecture, not a gap.
 *
 * So the full pipeline is:
 *   1. submit ACTION_READ_FILE with the path as the payload  -> action_id
 *   2. poll for the verdict                                  -> AE_APPROVED
 *   3. ONLY THEN open/read the file with our own authority
 *   4. verify the bytes
 *
 * Step 3 is the point. An agent never holds the authority to act; it proposes,
 * the kernel rules, and the agent executes afterwards. A probe that read the
 * file first and submitted afterwards would print the same sentinel and prove
 * nothing, which is why the gate asserts the CONTENT and not just the OK line.
 *
 * ACTION_READ_FILE is NOT in aether_action_forces_pending() (that set is
 * SPAWN_PROCESS / DELETE_FILE / REWRITE_AGENT_CODE / EVOLVE_GENOME), so in
 * sovereign mode it auto-approves and this probe needs no second, privileged
 * actor. The four force-pending types DO need one, and their gates must assert
 * the action stays PENDING -- DDR-1013 §2.1 records that split so nobody writes
 * a gate asserting the opposite of DDR-842's design.
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
#define SYS_SUBMIT_ACTION 31
#define SYS_POLL_RESULT   32

#define O_RDONLY 0x0

/* Wire format. aether.h pins these with _Static_assert -- ACTION_READ_FILE is
 * asserted == 5 there in the same commit as this file, because DDR-1013 §1
 * found actiondagtest.c had drifted to a wrong constant and no gate could see
 * it. If the enum ever shifts, the KERNEL fails to build. */
#define ACTION_READ_FILE 5
#define AE_PENDING       1
#define AE_APPROVED      2

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
    wr("ACTIONREAD FAIL: "); wr(why); wr(" rc="); wrdec(v); wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

/* force_align_arg_pointer: a process entry point is entered with RSP 16-byte
 * aligned, but the compiler assumes the call convention's RSP == 8 (mod 16).
 * Without it the frame is off by 8 and the first aligned SSE stack access #GPs.
 * DDR-1016 caught this by running ci-start-align-check, which DDR-1015 did not:
 * CLAUDE.md's hygiene list names only two of the three static checks, and CI
 * runs all three (ci.yml:35). Run tools/ci/hygiene_check.sh, not the list. */
__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    static const char path[] = "/HELLO.TXT";

    /* 1. PROPOSE. The payload is the path the action concerns. */
    long id = nsi(SYS_SUBMIT_ACTION, ACTION_READ_FILE, (long)path, slen(path));
    if (id < 0)
        fail("submit", id);

    /* 2. WAIT FOR THE VERDICT. Bounded: a poll that never terminates would turn
     * a policy bug into a gate timeout, which reads as something else entirely. */
    long st = -1;
    for (int i = 0; i < 20000; i++) {
        st = nsi(SYS_POLL_RESULT, id, 0, 0);
        if (st != AE_PENDING)
            break;
        nsi(SYS_YIELD, 0, 0, 0);
    }
    if (st != AE_APPROVED)
        fail("verdict", st);

    /* 3. EXECUTE, and only now. */
    long fd = nsi(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0)
        fail("open", fd);
    char buf[64];
    long n = nsi(SYS_READ, fd, (long)buf, (long)sizeof buf);
    nsi(SYS_CLOSE, fd, 0, 0);
    if (n <= 0)
        fail("read", n);

    /* 4. VERIFY THE BYTES. This is what makes the gate non-vacuous: a probe that
     * skipped step 3 still prints an OK line, but cannot print the content. */
    { uline u; ul_init(&u);                       /* DDR-1056: ONE write */
      ul_s(&u, "PRADYOS_ACTIONREAD_OK id="); ul_d(&u, id);
      ul_s(&u, " n=");     ul_d(&u, n);
      ul_s(&u, " first="); ul_c(&u, buf[0]);
      ul_s(&u, "\n"); wr(ul_end(&u)); }
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
