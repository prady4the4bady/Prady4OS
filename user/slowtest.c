/* user/slowtest.c — a background job that is still running when `wait` runs
 * (DDR-1068).
 *
 * WHY THIS EXISTS AT ALL, because a probe whose only job is to be slow looks
 * gratuitous until the alternative is stated. `wait` blocks until the shell's
 * background jobs finish, and DDR-1068 §3 measured that the obvious gate for it
 * is VACUOUS in two different ways:
 *
 *   - An ordering arm (`run X & ; wait ; echo MARKER`) is ONE-SIDED: with `wait`
 *     the ordering is guaranteed, without it the ordering is merely LIKELY, so
 *     it cannot separate "wait worked" from "the child happened to be fast".
 *   - A count arm (`reaped=N`) is worse: jobs_reap() runs at EVERY prompt and
 *     /EXECTEST.ELF exits in milliseconds, so by the time the `wait` line is
 *     typed a CORRECT `wait` legitimately reports reaped=0 — the same value a
 *     missing `wait` produces.
 *
 * Both failures have one cause: no probe in the tree is still running when the
 * next line is typed. This one is, by construction, and that is its entire
 * purpose.
 *
 * DURATION IS WALL TIME, NOT TICKS. SYS_CLOCK returns seconds and is what
 * DDR-1029 used to correct DDR-1028's mistake of reading g_ticks buckets as wall
 * seconds — under TCG the nominal 100 Hz does not hold, so a tick count is not a
 * duration. One-second resolution is coarse and adequate: the quantity is a
 * multi-second margin, not a measurement.
 *
 * IT YIELDS RATHER THAN SPINNING. A ring-3 busy loop would burn a CPU for the
 * whole interval on a machine whose known failure modes are timing-sensitive
 * (OPEN-2), and DDR-1047 refused a far smaller perturbation for that reason.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld gives a
 * single R+X PT_LOAD, so any writable section links fine and FAULTS ON FIRST
 * STORE — DDR-826).
 */

#include "uline.h"          /* DDR-1056: one write per measured line */

#define SYS_WRITE    6
#define SYS_EXIT     4
#define SYS_YIELD    3
#define SYS_CLOCK   57      /* () -> seconds since midnight (RTC) */

/* Long enough that the shell's next input line cannot outrun it: smoke-shell's
 * injector paces at 0.5-1.5 s per line, so 4 s clears the widest of those with
 * margin. Not longer, because every gate that boots this image pays it. */
#define SLOW_SECONDS 4

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    long t0 = nsi(SYS_CLOCK, 0, 0, 0);
    long waited = 0;

    /* BOUNDED TWO WAYS, because SYS_CLOCK is a wall clock and wall clocks wrap:
     * it returns seconds since midnight, so a boot that straddles midnight makes
     * (now - t0) negative and a naive loop would never terminate. The iteration
     * cap is what actually guarantees termination; the clock decides the normal
     * case. An unbounded wait here would turn a clock quirk into a gate timeout,
     * which reads as something else entirely (DDR-961/994). */
    for (long i = 0; i < 20000000L; i++) {
        long now = nsi(SYS_CLOCK, 0, 0, 0);
        waited = now - t0;
        if (waited < 0)                 /* midnight wrap: stop, do not spin */
            break;
        if (waited >= SLOW_SECONDS)
            break;
        nsi(SYS_YIELD, 0, 0, 0);
    }

    { uline u; ul_init(&u);
      ul_s(&u, "PRADYOS_SLOW_DONE waited=");
      ul_d(&u, waited);
      ul_s(&u, "\n"); wr(ul_end(&u)); }

    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
