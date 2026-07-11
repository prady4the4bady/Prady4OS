/* user/agentmetricstest.c — per-agent live metrics probe (DDR-730).
 *
 * A tiny observability witness: polls SYS_AGENT_METRICS until it sees the daemon's
 * test agent (roster slot 0 = KRYOS) reported ALIVE (state >= 1) while an unspawned
 * slot (7 = SOLIN) reads inactive — proving the metrics reflect real per-agent tcb
 * state (a running pid), not a stuck roster bit and not a blanket "all active".
 *
 *   -> AGENT_METRIC KRYOS live pid ok  (slot 0 alive, slot 7 idle)
 *   -> PRADYOS_AGENT_METRICS_OK
 *
 * Freestanding (no libc) to stay inside the kernel-image budget: raw SYS_WRITE of
 * fixed strings, no printf, no writable globals (links against user/user.ld).
 */

#define SYS_WRITE          6
#define SYS_YIELD          3
#define SYS_EXIT           4
#define SYS_AGENT_METRICS  64

struct agent_metric { unsigned pid, state; unsigned long mem_used, actions; };

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn)) void _start(void) {
    /* Poll long enough for the daemon to spawn the test agent (it lands late under
     * loaded CI runners — same reason smoke-agents uses a 150 s timeout). */
    for (long i = 0; i < 4000000; i++) {
        struct agent_metric m[8];
        long n = nsi(SYS_AGENT_METRICS, (long)m, 8, 0);
        if (n >= 8 && m[0].state >= 1 && m[0].pid != 0 && m[7].state == 0) {
            wr("AGENT_METRIC KRYOS live pid ok\n");   /* slot 0 alive, slot 7 idle */
            wr("PRADYOS_AGENT_METRICS_OK\n");
            nsi(SYS_EXIT, 0, 0, 0);
            for (;;) { }
        }
        nsi(SYS_YIELD, 0, 0, 0);
    }
    /* Never observed a live agent — exit non-zero; the gate fails on the missing
     * sentinel rather than a false pass. */
    wr("AGENT_METRICS FAIL: no live agent observed\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}
