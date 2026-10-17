/* user/killblocktest.c — DDR-1090: is SIGKILL deliverable to a thread that is
 * blocked in an unbounded kernel wait?
 *
 * Both `signal_deliver` call sites are guarded by `(r->cs & 3) == 3`, so a
 * signal is acted on only when the interrupted frame is RING 3. A thread inside
 * `sys_io.c:338`'s pipe-read spin is in ring 0 at every timer IRQ, forever — so
 * `sig_pending |= 1<<SIGKILL` is recorded and never acted on, although
 * `signal.h` calls SIGKILL "unblockable terminate" and `sys_aether.c:281` (the
 * SOVEREIGN's kill switch on a runaway agent) says "terminated on its next IRQ
 * return".
 *
 * THE WAIT IS MADE DETERMINISTIC BY THE PIPE, not by timing: the child holds the
 * READ end and the parent keeps the WRITE end open and never writes, so
 * `pipe_writers() > 0` holds forever and the loop's own exit condition can never
 * be satisfied. No console injector, no sleep, no race to lose.
 *
 * THE DISCRIMINATING VALUE COMES FROM THE KERNEL (DDR-1066/1084): `reaped=` is
 * `wait4`'s return, which the probe cannot manufacture — a child that is still
 * blocked cannot be reaped, and a child the kernel terminated can.
 *
 *   pre-fix  -> PRADYOS_KILLBLOCK child=<pid> reaped=0
 *   post-fix -> PRADYOS_KILLBLOCK child=<pid> reaped=<pid>
 *
 * VACUITY, CHECKED BEFORE THIS WAS WRITTEN (DDR-1090 §4): `reaped=0` is ALSO
 * what a correct kernel prints if the child never reached the block at all, so
 * the child prints PRADYOS_KILLBLOCK_CHILD *first* and the gate requires both.
 * Without that arm, "the kill was deferred" and "the fork lost the race" are the
 * same observation.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_READ   5
#define SYS_WRITE  6
#define SYS_CLOSE  8
#define SYS_EXIT   4
#define SYS_FORK  15
#define SYS_WAIT4 16
#define SYS_PIPE  17
#define SYS_KILL  23
#define SYS_YIELD  3
#define SYS_CLOCK 57      /* () -> seconds since midnight (RTC) */

#define SIGKILL    9
#define WNOHANG    1

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

static char *dec(char *p, long v) {
    if (v < 0) { *p++ = '-'; v = -v; }
    char t[20]; int n = 0;
    do { t[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

/* WALL TIME, NOT AN INSTRUCTION COUNT — DDR-1068/DDR-1029's precedent, and it
 * is why this probe works at all: the first draft used a fixed `pause` count,
 * and 200,000,000 iterations under TCG outran the gate's whole 120 s window, so
 * the parent never reached the kill and the capture showed only the child's
 * marker. An instruction count is a guess about emulation speed; seconds are
 * not. BOUNDED TWO WAYS because SYS_CLOCK is a wall clock and wall clocks wrap
 * at midnight: the iteration cap is what guarantees termination, the clock
 * decides the normal case. This probe does not hold is_agent, so
 * AETHER_RATE_MAX does not apply to the polling (DDR-1016 §4). */
static void waitsec(long secs) {
    long t0 = nsi(SYS_CLOCK, 0, 0, 0);
    for (long i = 0; i < 2000000; i++) {
        long now = nsi(SYS_CLOCK, 0, 0, 0);
        long waited = now - t0;
        if (waited < 0)                  /* midnight wrap: stop, do not spin */
            return;
        if (waited >= secs)
            return;
        nsi(SYS_YIELD, 0, 0, 0);
    }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    int fds[2];
    if (nsi(SYS_PIPE, (long)fds, 0, 0) != 0) {
        wr("KILLBLOCK FAIL: pipe\n");
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }

    long kid = nsi(SYS_FORK, 0, 0, 0);
    if (kid < 0) {
        wr("KILLBLOCK FAIL: fork\n");
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }

    if (kid == 0) {
        /* CHILD. Drop the write end so only the PARENT holds it: the parent
         * never writes, so this read can never be satisfied and never sees EOF.
         * The marker goes out BEFORE the block — it is the arm that separates
         * "deferred kill" from "never got here". */
        nsi(SYS_CLOSE, fds[1], 0, 0);
        wr("PRADYOS_KILLBLOCK_CHILD blocking\n");
        char b[8];
        nsi(SYS_READ, fds[0], (long)b, 1);       /* unbounded: sys_io.c:338 */
        /* Reached only if the read somehow returns; the parent's arm is what
         * the gate judges, so this exits quietly rather than fail()ing. */
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }

    /* PARENT. Keep the write end OPEN — that is what makes the child's wait
     * unbounded — and give the child time to print its marker and enter the
     * kernel loop before the kill lands. */
    waitsec(3);
    nsi(SYS_KILL, kid, SIGKILL, 0);
    waitsec(3);                                  /* ~300 timer IRQs land here */

    long st = 0;
    long reaped = nsi(SYS_WAIT4, kid, (long)&st, WNOHANG);

    {   char line[80]; char *p = line;
        const char *tag = "PRADYOS_KILLBLOCK child=";
        for (int i = 0; tag[i]; i++) *p++ = tag[i];
        p = dec(p, kid);
        *p++ = ' '; *p++ = 'r'; *p++ = 'e'; *p++ = 'a'; *p++ = 'p';
        *p++ = 'e'; *p++ = 'd'; *p++ = '=';
        p = dec(p, reaped);
        *p++ = '\n';
        nsi(SYS_WRITE, 1, (long)line, p - line);
    }
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
