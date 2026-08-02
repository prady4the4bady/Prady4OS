/* user/agentmetricstest.c — per-agent live metrics probe (DDR-730/735).
 *
 * Proves SYS_AGENT_METRICS reflects real per-agent scheduling, using facts that
 * are POST-MORTEM STABLE: the kernel captures an agent's final CPU counters at
 * exit (agent_metrics_reap) and retains its pid in the roster slot, so this
 * probe does NOT need to catch the agent alive — on slow TCG CI runners an
 * agent's whole life can fit between two probe samples (a seconds-long
 * compositor quantum), which made the original alive-window assertion racy.
 *
 * Latches, in any order, within an RTC-bounded window:
 *   - slot 0 (KRYOS): pid != 0 AND dispatches >= 1 — the daemon's agent was
 *     spawned and provably scheduled (readable during OR after its life)
 *                                            -> AGENT_METRIC KRYOS sched ok
 *   - slot 7 (SOLIN): stays pid == 0, dispatches == 0 — the metric
 *     discriminates spawned slots from empty ones (not a blanket all-active)
 *     (checked at latch time, not separately printed)
 *   - opportunistic: if slot 0 is also caught alive (state >= 1), print the
 *     extra "live pid ok" line — informative, NOT required by the gate.
 * Both facts -> PRADYOS_AGENT_METRICS_OK. Window exhausted -> an INFORMATIONAL
 * line and exit 0 (DDR-798): this probe runs on every boot, so a deadline it
 * declares itself is a verdict about gates that never set it. smoke-agentmetrics
 * owns the assertion via its required sentinels.
 *
 * The window is 120 RTC seconds (SYS_CLOCK, seconds-since-midnight; wrap
 * handled) — iteration-count bounds scale with host speed and mis-size on TCG.
 *
 * Freestanding (no libc): user.ld single R+X segment, no writable globals.
 */

#define SYS_WRITE          6
#define SYS_YIELD          3
#define SYS_EXIT           4
#define SYS_CLOCK          57
#define SYS_AGENT_METRICS  64

struct agent_metric { unsigned pid, state; unsigned long mem_used, actions, run_ticks, dispatches; };

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

static long elapsed(long start) {
    long now = nsi(SYS_CLOCK, 0, 0, 0);
    if (now < start) now += 86400;             /* midnight wrap */
    return now - start;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    long start = nsi(SYS_CLOCK, 0, 0, 0);
    int live_said = 0;
    while (elapsed(start) < 120) {
        struct agent_metric m[8];
        long n = nsi(SYS_AGENT_METRICS, (long)m, 8, 0);
        if (n >= 8) {
            if (!live_said && m[0].state >= 1 && m[0].pid != 0) {
                live_said = 1;                 /* bonus observation, not required */
                wr("AGENT_METRIC KRYOS live pid ok\n");
            }
            if (m[0].pid != 0 && m[0].dispatches >= 1 &&
                m[7].pid == 0 && m[7].dispatches == 0) {
                wr("AGENT_METRIC KRYOS sched ok\n");   /* spawned + provably ran */
                wr("PRADYOS_AGENT_METRICS_OK\n");
                nsi(SYS_EXIT, 0, 0, 0);
                for (;;) { }
            }
        }
        nsi(SYS_YIELD, 0, 0, 0);
    }
    /* DDR-798: window expiry is NOT a verdict this probe may return.
     *
     * This probe runs on EVERY boot, but its 120-second deadline is only
     * meaningful in smoke-agentmetrics — the one gate that asks whether the
     * agent gets scheduled and gives the boot 150 s to show it. In the other
     * ~106 gates the probe still counted to 120 and declared failure about a
     * boot that was never trying to reach the daemon quickly, so on a slow TCG
     * runner it failed for reasons unrelated to what it tests. DDR-791's
     * GLOBAL_FORBIDDEN then correctly propagated that to whichever gate was
     * running, which is how it surfaced.
     *
     * The assertion has not been dropped — it has moved to where a timeout
     * belongs. smoke-agentmetrics REQUIRES "AGENT_METRIC KRYOS sched ok" and
     * PRADYOS_AGENT_METRICS_OK, so a genuinely unscheduled agent fails that gate
     * on a missing required sentinel, which cannot be masked.
     *
     * Informational rather than silent: an observation that stops being reported
     * is how a regression becomes invisible. This string is deliberately NOT a
     * failure pattern and must never be added to GLOBAL_FORBIDDEN. */
    wr("AGENT_METRIC not observed in window (informational; gate decides)\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
