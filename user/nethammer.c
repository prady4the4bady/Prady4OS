/* user/nethammer.c — the two-CPU connect/close hammer (DDR-990).
 *
 * WHY THIS EXISTS. DDR-987 found a real cross-CPU use-after-free in the lwIP
 * core and fixed it with g_net_lock, but DDR-987 §5 recorded that the gate
 * suite CANNOT prove that fix: the defect surfaces through smoke-surfdestroy at
 * ~1 run in 20, so a clean 20-run campaign has only ~64% power (0.95^20 ≈ 0.358
 * — a clean sweep happens one time in three even if the defect is untouched).
 * Sampling to 95% confidence would need ~59 boots. That is the wrong
 * instrument, and DDR-990 §1 has the arithmetic.
 *
 * This probe is the right one. The defect has a NAMED MECHANISM: cpu A walks
 * pcb->unsent inside tcp_output, reached from sys_sock_connect, while cpu B
 * frees that seg via tcp_abort from psock_close. So instead of waiting for an
 * unrelated gate to trip the race incidentally, two instances of this probe
 * drive exactly that shape on two CPUs, with no phase relationship between
 * them, as fast as ring 3 can issue the syscalls.
 *
 * THE PASS CRITERION IS NOT "IT PRINTED OK". Read DDR-990 §4 before trusting a
 * green run. A hammer that survives on a kernel with g_net_lock REVERTED proves
 * nothing at all — that is DDR-988 §9's vacuous gate (smoke-net-fuzz was green
 * while 613 of its 768 frames never reached lwIP, because its criterion was
 * survival and dropping a frame survives reliably). This probe is only evidence
 * once it has been mutation-checked in BOTH directions.
 *
 * conn_err IS LOAD-BEARING, not decoration. If the egress allowlist does not
 * carry 127.0.0.1:8007, every connect returns an audited -EPERM, the probe
 * hammers NOTHING, and it still reaches its sentinel — a green result from a
 * probe that never touched lwIP. main.c seeds that allowlist row in the same
 * gated block that spawns this, and the gate asserts conn_err=0. That assertion
 * is the difference between a measurement and a decoration.
 *
 * Freestanding (no libc, user.ld, no writable globals per DDR-826). All state
 * is on the stack, inside the 8 eager pages of ADR-038.
 */

#define SYS_GETPID        2
#define SYS_EXIT          4
#define SYS_WRITE         6
#define SYS_SOCK_CONNECT  39
#define SYS_SOCK_CLOSE    42

/* Loopback to the in-kernel TCP echo server net_init() binds (lwip_port.c:374).
 * Loopback needs no hardware and still drives the full tcp_connect ->
 * tcp_output -> seg-alloc path, which is the half of the race that allocates. */
#define DEST_HOST_BE  0x7F000001u      /* 127.0.0.1, big-endian as the NSI takes it */
#define DEST_PORT     8007

/* Iterations per instance. DDR-990 §4 requires this be chosen at ~10x the
 * iteration count at which the REVERTED kernel first faults, so that a clean
 * run on the fixed kernel carries real power. PROGRESS_EVERY exists to make
 * that count measurable: the last progress line before a panic is how far the
 * unfixed kernel got. */
#define ITERS          20000
#define PROGRESS_EVERY 1000

static inline long nsi4(long n, long a1, long a2, long a3, long a4) {
    long r;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}
static inline long nsi(long n, long a1, long a2, long a3) { return nsi4(n, a1, a2, a3, 0); }

/* DDR-990 §13: which CPU is this instance on, as a bit. CPUID leaf 1 returns
 * the initial APIC ID in EBX[31:24] and is NOT privileged, so a ring-3 probe
 * can read it with no syscall and no kernel change. Capped at bit 7 because the
 * mask is printed as a decimal and QEMU_SMP here is 4 — a wider machine would
 * still be reported, just saturated, which is honest rather than wrapping onto
 * another CPU's bit. */
static inline unsigned long cpu_bit(void) {
    unsigned int a = 1, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    unsigned apic = (b >> 24) & 0xFFu;
    if (apic > 7u) apic = 7u;
    return 1UL << apic;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

/* DDR-990 §14: a LINE BUILDER, because one field per write() is not atomic.
 *
 * Measured, not anticipated: pid=38's completion line came back as
 *   PRADYOS_NETHAMMER_OK pid=38 iters=20000 conn_ok=20000 conn_err=[user] ELF …
 * — a kernel print landed between this probe's `conn_err=` write and the value
 * that belonged after it. Six writes per line under SMP means five windows for
 * another CPU to interleave (§INV.23).
 *
 * That is not merely cosmetic. The gate's `conn_err=0` sentinel is a substring
 * search over the whole log, so a truncated line contributes nothing and the
 * check still passes on the OTHER instance's copy — the exact "one clean
 * instance satisfying the check while the other errored" hole this probe's own
 * comment set out to close, reopened by the console rather than by the logic.
 *
 * One write per line closes the windows this probe controls. It does not make
 * the console globally atomic — a single long line could still in principle be
 * split — but it removes the five self-inflicted seams. */
struct lb { char b[192]; int n; };
static void lb_s(struct lb *l, const char *s) {
    while (*s && l->n < (int)sizeof l->b - 1) l->b[l->n++] = *s++;
}
static void lb_d(struct lb *l, unsigned long v) {
    char t[24]; int i = 24;
    if (!v) t[--i] = '0';
    while (v) { t[--i] = (char)('0' + (v % 10u)); v /= 10u; }
    while (i < 24 && l->n < (int)sizeof l->b - 1) l->b[l->n++] = t[i++];
}
static void lb_flush(struct lb *l) {
    if (l->n < (int)sizeof l->b) l->b[l->n++] = '\n';
    nsi(SYS_WRITE, 1, (long)l->b, l->n);      /* ONE syscall, whole line */
    l->n = 0;
}

/* Decimal, into a caller-supplied stack buffer — no writable globals. */
static void wrdec(unsigned long v) {
    char b[24];
    int i = 23;
    b[i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + (v % 10u)); v /= 10u; }
    nsi(SYS_WRITE, 1, (long)&b[i], slen(&b[i]));
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    unsigned long pid = (unsigned long)nsi(SYS_GETPID, 0, 0, 0);
    unsigned long conn_ok = 0, conn_err = 0;
    unsigned long cpu_mask = 0;      /* DDR-990 §13 */

    wr("NETHAMMER_START pid="); wrdec(pid);
    wr(" iters="); wrdec(ITERS);
    wr("\n");

    for (unsigned long i = 0; i < ITERS; i++) {
        long fd = nsi(SYS_SOCK_CONNECT, (long)DEST_HOST_BE, DEST_PORT, 0);
        if (fd >= 0) {
            conn_ok++;
            /* Close IMMEDIATELY, without reading or writing. The teardown is
             * the half of the race that FREES, and issuing it while the peer
             * side of the loopback connection is still being set up on another
             * CPU is precisely the interleaving DDR-987 named. */
            nsi(SYS_SOCK_CLOSE, fd, 0, 0);
        } else {
            /* Count, do not abort. A run that is all-error must still reach the
             * sentinel so the gate can catch it via conn_err rather than via a
             * timeout that looks like something else. */
            conn_err++;
        }
        /* DDR-990 §13 (CodeRabbit, PR #14): sample WHICH cpu this instance is
         * running on. The gate asserted two distinct PIDs, which proves two
         * instances finished — it does NOT prove they ever ran at the same
         * time on different CPUs, and a two-CPU race probe that both instances
         * run on one CPU is a green gate testing nothing.
         *
         * CPUID leaf 1, EBX[31:24] is the initial APIC ID, and CPUID is not a
         * privileged instruction — so this needs no syscall and no kernel
         * change. Sampled every iteration and accumulated into a MASK because
         * a thread can migrate: one reading at exit would report where it
         * ended, not where it ran. */
        cpu_mask |= cpu_bit();
        if (((i + 1) % PROGRESS_EVERY) == 0) {
            wr("NETHAMMER_PROG pid="); wrdec(pid);
            wr(" i="); wrdec(i + 1);
            wr("\n");
        }
    }

    /* Self-assert BEFORE claiming success. The gate can only test that the
     * string "conn_err=0" appears SOMEWHERE, which one clean instance would
     * satisfy while the other errored out — a green gate hiding a half-broken
     * run. Each instance therefore checks its own counters, so a failure is
     * attributable to a pid instead of merely present in the boot. */
    if (conn_err != 0) {
        struct lb l; l.n = 0;
        lb_s(&l, "NETHAMMER FAIL: pid="); lb_d(&l, pid);
        lb_s(&l, " conn_err="); lb_d(&l, conn_err);
        lb_s(&l, " — the hammer never entered lwIP (allowlist row missing?)");
        lb_flush(&l);
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }
    if (conn_ok != ITERS) {
        struct lb l; l.n = 0;
        lb_s(&l, "NETHAMMER FAIL: pid="); lb_d(&l, pid);
        lb_s(&l, " conn_ok="); lb_d(&l, conn_ok);
        lb_s(&l, " != iters");
        lb_flush(&l);
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }

    /* DDR-990 §14: one write, not six. */
    struct lb l; l.n = 0;
    lb_s(&l, "PRADYOS_NETHAMMER_OK pid="); lb_d(&l, pid);
    lb_s(&l, " iters=");    lb_d(&l, ITERS);
    lb_s(&l, " conn_ok=");  lb_d(&l, conn_ok);
    lb_s(&l, " conn_err="); lb_d(&l, conn_err);
    lb_s(&l, " cpumask=");  lb_d(&l, cpu_mask);      /* DDR-990 §13 */
    lb_flush(&l);
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
