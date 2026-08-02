/* user/sigpipetest.c — DDR-805 SIGPIPE probe.
 *
 * The discriminating property is that a writer to a READERLESS pipe must not
 * survive its own write. That needs no new exit-status encoding: the kernel sets
 * exit_status = -1 for every default-terminate signal and records no signal
 * number anywhere, so asserting "$? == 13" would require a fourth edit changing
 * SIGKILL/SIGTERM too. Surviving-or-not is observable today and is exactly the
 * behaviour this DDR changes.
 *
 * Two phases, control FIRST because the second phase is expected to kill us:
 *
 *   CONTROL — pipe with the read end still OPEN. The write must succeed and we
 *             must live to print PRADYOS_SIGPIPE_CONTROL_OK. This is what proves
 *             the kill is specific to the readerless case rather than a blanket
 *             failure of pipe writes; without it, a kernel that killed every
 *             pipe writer would pass the main assertion.
 *
 *   READERLESS — read end closed, then write. SIGPIPE must terminate us BEFORE
 *             the next line executes. If it does not, we print
 *             PRADYOS_SIGPIPE_STUB, which is the gate's FORBIDDEN_SENTINEL.
 *
 * That sentinel is load-bearing, not decorative: printing it IS what surviving
 * means, so a stub cannot fake a pass by emitting the success marker — it would
 * have to also not print the failure marker, which only a real kill achieves.
 *
 * Delivery is at the next IRQ return to ring 3 (idt.c), not at the syscall
 * boundary, so write() returns -EPIPE first and we die a tick later. The spin
 * below is bounded (S2) and exists to give that tick somewhere to land.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_EXIT   4
#define SYS_WRITE  6
#define SYS_CLOSE  8
#define SYS_PIPE  17

#define EPIPE 32

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn)) static void fail(const char *why) {
    wr("SIGPIPE FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    int fds[2];

    /* ---- CONTROL: reader stays open, the write must succeed and we must live. */
    if (nsi(SYS_PIPE, (long)fds, 0, 0) != 0)
        fail("SYS_PIPE failed (control)");
    if (nsi(SYS_WRITE, fds[1], (long)"ctl", 3) != 3)
        fail("write to a pipe with a LIVE reader did not succeed");
    wr("PRADYOS_SIGPIPE_CONTROL_OK\n");
    nsi(SYS_CLOSE, fds[0], 0, 0);
    nsi(SYS_CLOSE, fds[1], 0, 0);

    /* ---- READERLESS: close the read end, then write. SIGPIPE must kill us. */
    if (nsi(SYS_PIPE, (long)fds, 0, 0) != 0)
        fail("SYS_PIPE failed (readerless)");
    if (nsi(SYS_CLOSE, fds[0], 0, 0) != 0)
        fail("closing the read end failed");

    long r = nsi(SYS_WRITE, fds[1], (long)"x", 1);
    if (r != -EPIPE)
        fail("readerless write did not return -EPIPE");

    /* -EPIPE arrived, which the pre-DDR-805 kernel also did. The DIFFERENCE is
     * that we must not get any further. Bounded spin (S2) so the pending signal
     * has a timer tick to be delivered on; if it never is, we fall through and
     * print the forbidden marker, which is the gate failing honestly. */
    for (volatile unsigned i = 0; i < 40000000u; i++) { }

    wr("PRADYOS_SIGPIPE_STUB\n");   /* reached ONLY if SIGPIPE did not kill us */
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
