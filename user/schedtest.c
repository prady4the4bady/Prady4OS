/* user/schedtest.c — ring-3 probe for smoke-schedclass (DDR-1188 §4).
 *
 * C1.5 P1 arms (DDR-1188 §4 and Amendment 1):
 *   A  CLOCK_MONOTONIC (the vDSO TSC clock) never goes backwards: on each of
 *      two threads pinned to CPU 0 and CPU 1 (>= 10 000 reads and >= 2 s each),
 *      and across them: 10 000 rounds of a ping-pong in which each CPU in turn
 *      publishes a reading and the other reads it, then its own clock, which
 *      must not be below it (both directions). Also: the syscall agrees with
 *      the vDSO (a syscall reading lies between two vDSO readings), and
 *      CLOCK_REALTIME is past 2023 (the RTC base was applied).
 *      Mutant C1 (A1-2): ns_base advanced a fixed 10 ms per tick.
 *   B  nanosleep(25 ms) takes >= 25 ms by CLOCK_MONOTONIC, 5 times of 5;
 *      three malformed timespecs return -EINVAL.
 *      Mutant C2: the sleep rounded down to whole ticks.
 *
 * `[sched]` lines are data; the PRADYOS_SCHED_* lines are the sentinels, each
 * emitted by one write (DDR-1182 §3.1). QEMU (TCG) only; not verified on real
 * hardware.
 */
#include "pradyos.h"

#define nsi pradyos_syscall

#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_ANONYMOUS  0x20
#define EINVAL         22
#define EINTR          4
#define SIGKILL        9
#define SIGUSR1        10
#define SIGTERM        15
#define SA_SIGINFO     0x00000004ul
#define CLK_REALTIME   0
#define CLK_MONOTONIC  1

#define VDSO_USER_VA   0x00007FFFFFF00000ull
#define NS_PER_SEC     1000000000ull
#define MS             1000000ull
#define STK            16384
#define A_READS_MIN    10000
#define A_NS_MIN       (2 * NS_PER_SEC)
#define X_ROUNDS       10000
#define B_SLEEPS       5
#define B_NS           (25 * MS)

/* The vDSO record (kernel/vdso/vdso_page.h, DDR-1188 K7). */
struct vdso {
    volatile uint64_t wall_time_ns;
    volatile uint32_t seq;
    volatile uint32_t mult;
    volatile uint64_t tsc_base;
    volatile uint64_t ns_base;
    volatile uint64_t realtime_base;
    volatile uint32_t shift;
};
_Static_assert(__builtin_offsetof(struct vdso, mult) == 12, "vdso mult");
_Static_assert(__builtin_offsetof(struct vdso, tsc_base) == 16, "vdso tsc_base");
_Static_assert(__builtin_offsetof(struct vdso, ns_base) == 24, "vdso ns_base");
_Static_assert(__builtin_offsetof(struct vdso, realtime_base) == 32, "vdso realtime_base");
_Static_assert(__builtin_offsetof(struct vdso, shift) == 40, "vdso shift");

struct ts { long sec, nsec; };

static inline long nsi6(long n, long a1, long a2, long a3,
                        long a4, long a5, long a6) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

static inline uint64_t rdtsc(void) {             /* DDR-1188 A2-2: ordered */
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* CLOCK_MONOTONIC from the vDSO: the kernel's own reader (ktime_mono_ns). */
static uint64_t mono(void) {
    const struct vdso *v = (const struct vdso *)VDSO_USER_VA;
    for (;;) {
        uint32_t s0 = __atomic_load_n(&v->seq, __ATOMIC_ACQUIRE);
        if (s0 & 1u)
            continue;
        uint32_t mult = v->mult;
        uint64_t r;
        if (!mult) {
            r = v->wall_time_ns;
        } else {
            uint64_t base = v->ns_base, tb = v->tsc_base;
            uint32_t sh = v->shift;
            uint64_t now = rdtsc();
            uint64_t d = now > tb ? now - tb : 0;
            r = base + (uint64_t)(((unsigned __int128)d * mult) >> sh);
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&v->seq, __ATOMIC_RELAXED) == s0)
            return r;
    }
}

static void say(const char *fmt, ...) {
    char b[192];
    long n = 0;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (const char *f = fmt; *f && n < (long)sizeof b - 21; f++) {
        if (f[0] == '%' && f[1] == 'd') {
            long v = __builtin_va_arg(ap, long);
            unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
            char d[20];
            int nd = 0;
            do { d[nd++] = (char)('0' + u % 10); u /= 10; } while (u);
            if (v < 0) b[n++] = '-';
            while (nd) b[n++] = d[--nd];
            f++;
        } else if (f[0] == '%' && f[1] == 's') {
            for (const char *s = __builtin_va_arg(ap, const char *);
                 *s && n < (long)sizeof b - 1; s++)
                b[n++] = *s;
            f++;
        } else {
            b[n++] = *f;
        }
    }
    __builtin_va_end(ap);
    nsi(SYS_WRITE, 1, (long)b, n);
}

__attribute__((noreturn)) static void fail(const char *why) {
    say("SCHED FAIL: %s\n", why);
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn)) static void texit(void) {
    nsi(SYS_THREAD_EXIT, 0, 0, 0);
    for (;;) { }
}

static long spawn(void (*fn)(long), long arg) {
    long stk = nsi6(SYS_MMAP, 0, STK, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stk < 0) fail("mmap");
    long tid = nsi6(SYS_THREAD_CREATE, (long)fn, stk + STK, arg, 0, 0, 0);
    if (tid < 0) fail("create");
    return tid;
}

/* ---- arm A ---- */

/* Shared by the two threads through a mapped page: the probe has no writable
 * segment (probe-rodata-check), so no mutable globals. A thread's argument is
 * the page address with its index (0 or 1) in bit 0. */
struct a_state {
    volatile long reads[2], back[2], pinned[2], done[2];
    volatile long ready;                 /* threads that finished phase 1   */
    volatile uint64_t seq;               /* ping-pong: odd = 0->1, even = 1->0 */
    volatile uint64_t val;               /* the published reading           */
    volatile long xback, xrounds, xtimeout;
};

/* Pin to cpu and wait (bounded) until running there. */
static int pin(long cpu) {
    if (nsi(SYS_SCHED_CPU, 1, cpu, 0) != 0)
        return 0;
    for (int i = 0; i < 1000; i++) {
        if (nsi(SYS_SCHED_CPU, 0, 0, 0) == cpu)
            return 1;
        nsi(SYS_YIELD, 0, 0, 0);
    }
    return 0;
}

/* Spin until a->seq == want; 0 after 5 s of CLOCK_MONOTONIC. */
static int await_seq(struct a_state *a, uint64_t want) {
    uint64_t end = mono() + 5 * NS_PER_SEC;
    while (__atomic_load_n(&a->seq, __ATOMIC_ACQUIRE) != want) {
        if (mono() > end) return 0;
        __asm__ volatile("pause");
    }
    return 1;
}

__attribute__((noreturn, force_align_arg_pointer)) static void a_thread(long arg) {
    long me = arg & 1;
    struct a_state *a = (struct a_state *)(arg & ~1L);
    a->pinned[me] = pin(me);
    /* Phase 1: same-CPU monotonicity. */
    uint64_t start = mono(), prev = start;
    long reads = 0, back = 0;
    for (;;) {
        uint64_t t = mono();
        if (t < prev) back++;
        prev = t;
        reads++;
        if (reads >= A_READS_MIN && t - start >= A_NS_MIN) break;
    }
    a->reads[me] = reads;
    a->back[me] = back;
    __atomic_add_fetch(&a->ready, 1, __ATOMIC_SEQ_CST);
    uint64_t bar = mono() + 10 * NS_PER_SEC;           /* both reach phase 2 */
    while (__atomic_load_n(&a->ready, __ATOMIC_SEQ_CST) < 2 && mono() < bar)
        __asm__ volatile("pause");
    /* Phase 2: cross-CPU ping-pong. Round r (1..2*X_ROUNDS): odd r is
     * 0 -> 1, even r is 1 -> 0. The sender publishes val, then seq = r; the
     * receiver waits for seq == r, reads its own clock, compares. */
    for (uint64_t r = 1; r <= 2 * X_ROUNDS; r++) {
        long sender = (r & 1) ? 0 : 1;
        if (me == sender) {
            if (!await_seq(a, r - 1)) { a->xtimeout = 1; break; }
            a->val = mono();
            __atomic_store_n(&a->seq, r, __ATOMIC_RELEASE);
        } else {
            if (!await_seq(a, r)) { a->xtimeout = 1; break; }
            uint64_t got = a->val;
            uint64_t mine = mono();
            if (mine < got) __atomic_add_fetch(&a->xback, 1, __ATOMIC_SEQ_CST);
            __atomic_add_fetch(&a->xrounds, 1, __ATOMIC_SEQ_CST);
        }
    }
    nsi(SYS_SCHED_CPU, 2, 0, 0);
    a->done[me] = 1;
    texit();
}

static void arm_a(void) {
    long pg = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pg < 0) fail("mmap");
    struct a_state *a = (struct a_state *)pg;   /* zero-filled */
    spawn(a_thread, pg);
    spawn(a_thread, pg | 1);
    uint64_t end = mono() + 60 * NS_PER_SEC;
    while (!(a->done[0] && a->done[1])) {
        if (mono() > end) fail("A threads did not finish");
        nsi(SYS_YIELD, 0, 0, 0);
    }
    /* syscall/vDSO agreement: v1 <= sys <= v2, 100 times. */
    long sys_ok = 1;
    for (int i = 0; i < 100; i++) {
        struct ts t;
        uint64_t v1 = mono();
        if (nsi(SYS_CLOCK_GETTIME, CLK_MONOTONIC, (long)&t, 0) != 0) { sys_ok = 0; break; }
        uint64_t v2 = mono();
        uint64_t s = (uint64_t)t.sec * NS_PER_SEC + (uint64_t)t.nsec;
        if (s < v1 || s > v2 || t.nsec < 0 || t.nsec >= (long)NS_PER_SEC) sys_ok = 0;
    }
    struct ts bad;
    if (nsi(SYS_CLOCK_GETTIME, 7, (long)&bad, 0) != -EINVAL) sys_ok = 0;
    struct ts rt;
    long rt_ok = nsi(SYS_CLOCK_GETTIME, CLK_REALTIME, (long)&rt, 0) == 0 &&
                 rt.sec > 1700000000L;
    long pin_ok = a->pinned[0] && a->pinned[1];
    long xrounds = a->xtimeout ? 0 : a->xrounds;
    say("[sched] A reads0=%d reads1=%d back0=%d back1=%d xrounds=%d xback=%d rt_sec=%d\n",
        a->reads[0], a->reads[1], a->back[0], a->back[1], xrounds, a->xback, rt.sec);
    if (xrounds != 2 * X_ROUNDS) fail("A cross-CPU rounds incomplete");
    say("PRADYOS_SCHED_A back=%d xback=%d sys_ok=%d pin_ok=%d rt_ok=%d\n",
        a->back[0] + a->back[1], a->xback, sys_ok, pin_ok, rt_ok);
}

/* ---- arm B2 (DDR-1188 A2-1): a signal ends a blocked nanosleep ---- */

struct k_sigaction { unsigned long handler, flags, mask; };

__attribute__((noreturn, force_align_arg_pointer))
static void on_usr1(long sig, void *si) {
    (void)sig; (void)si;
    nsi(SYS_SIGRETURN, 0, 0, 0);
    for (;;) { }
}

struct b2_state { volatile long ready, eintr, rem_ok; };

/* 1 once the scheduler reports pid BLOCKED (THREAD_BLOCKED = 3), 0 after 5 s. */
static int blocked(long pid) {
    struct { uint32_t pid, ppid, state, flags; char name[16];
             uint64_t run_ticks, dispatches; } pi;
    uint64_t end = mono() + 5 * NS_PER_SEC;
    for (;;) {
        for (long i = 0; i < 4096 && nsi(SYS_GETPROCS, i, (long)&pi, 0) == 1; i++)
            if ((long)pi.pid == pid && pi.state == 3)
                return 1;
        if (mono() > end) return 0;
        nsi(SYS_YIELD, 0, 0, 0);
    }
}

/* Fork a sleeper that marks ready and sleeps 5 s. Signal it once the scheduler
 * reports it BLOCKED, so the signal arrives during the block and not before
 * it (CodeRabbit on #130). Return the ms from the signal to its reaping, or
 * -1 if it never blocked, the kill failed, or wait4 did not reap it with
 * exit status want_st. */
static long b2_one(struct b2_state *b, long sig, int want_st) {
    b->ready = 0; b->eintr = 0; b->rem_ok = 0;
    long kid = nsi(SYS_FORK, 0, 0, 0);
    if (kid < 0) fail("B2 fork");
    if (kid == 0) {
        if (sig == SIGUSR1) {
            struct k_sigaction a = { (unsigned long)on_usr1, SA_SIGINFO, 0 };
            if (nsi(SYS_SIGACTION2, SIGUSR1, (long)&a, 0) != 0) fail("B2 sigaction");
        }
        struct ts req = { 5, 0 }, rem = { -1, -1 };
        b->ready = 1;
        long rc = nsi(SYS_NANOSLEEP, (long)&req, (long)&rem, 0);
        b->eintr = rc == -EINTR;
        b->rem_ok = rem.sec >= 0 && rem.nsec >= 0 && rem.nsec < (long)NS_PER_SEC &&
                    (rem.sec > 0 || rem.nsec > 0) && rem.sec < 5;
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }
    uint64_t end = mono() + 10 * NS_PER_SEC;
    while (!b->ready) {
        if (mono() > end) return -1;
        nsi(SYS_YIELD, 0, 0, 0);
    }
    int was_blocked = blocked(kid);
    uint64_t t0 = mono();
    long kill_rc = nsi(SYS_KILL, kid, sig, 0);
    int st = 0x7fffffff;                 /* SYS_WAIT4 writes an int */
    long reaped = nsi(SYS_WAIT4, kid, (long)&st, 0);
    if (!was_blocked || kill_rc != 0 || reaped != kid || st != want_st)
        return -1;
    return (long)((mono() - t0) / MS);
}

static void arm_b2(void) {
    long pg = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (pg < 0) fail("mmap");
    struct b2_state *b = (struct b2_state *)pg;
    long usr_ms = b2_one(b, SIGUSR1, 0);       /* the sleeper exits 0 itself */
    long eintr = b->eintr, rem_ok = b->rem_ok;
    long eintr_ok = usr_ms >= 0 && usr_ms < 2000 && eintr && rem_ok;
    long kill_ms = b2_one(b, SIGKILL, -1);     /* ended by the kernel: -1 */
    long killed_ok = kill_ms >= 0 && kill_ms < 2000;
    say("[sched] B2 usr1_ms=%d eintr=%d rem_ok=%d kill_ms=%d\n",
        usr_ms, eintr, rem_ok, kill_ms);
    say("PRADYOS_SCHED_B2 eintr_ok=%d killed_ok=%d\n", eintr_ok, killed_ok);
    /* B3 (DDR-1188 A2-3): SIGTERM with no handler ends the sleep, and so the
     * process, through default termination. */
    long term_ms = b2_one(b, SIGTERM, -1);
    say("[sched] B3 term_ms=%d\n", term_ms);
    say("PRADYOS_SCHED_B3 term_ok=%d\n", (long)(term_ms >= 0 && term_ms < 2000));
}

/* ---- arm B ---- */

static void arm_b(void) {
    long ok = 0;
    uint64_t mn = ~0ull, mx = 0;
    for (int i = 0; i < B_SLEEPS; i++) {
        struct ts req = { 0, (long)B_NS };
        uint64_t t0 = mono();
        long rc = nsi(SYS_NANOSLEEP, (long)&req, 0, 0);
        uint64_t d = mono() - t0;
        if (rc == 0 && d >= B_NS) ok++;
        if (d < mn) mn = d;
        if (d > mx) mx = d;
    }
    long einval = 0;
    struct ts b1 = { 0, -1 }, b2 = { 0, (long)NS_PER_SEC }, b3 = { -1, 0 };
    if (nsi(SYS_NANOSLEEP, (long)&b1, 0, 0) == -EINVAL) einval++;
    if (nsi(SYS_NANOSLEEP, (long)&b2, 0, 0) == -EINVAL) einval++;
    if (nsi(SYS_NANOSLEEP, (long)&b3, 0, 0) == -EINVAL) einval++;
    say("[sched] B min_us=%d max_us=%d\n", (long)(mn / 1000), (long)(mx / 1000));
    say("PRADYOS_SCHED_B slept_ok=%d einval=%d\n", ok, einval);
}

/* ---- arm C (DDR-1188 Amendment 3): a one-shot ends a sleep between ticks ----
 * The vDSO's wall_time_ns advances once per tick, in the handler that advances
 * g_ticks. A tick-timed sleep ends only after g_ticks moved, so a 2 ms sleep
 * across which wall_time_ns did not move was ended by a one-shot. */
#define C_TRIES  50
#define C_NS     (2 * MS)
#define C_MIN    20
#define C_CPU    2

static void arm_c(void) {
    const struct vdso *v = (const struct vdso *)VDSO_USER_VA;
    long early = 0, slept = 0;
    long pinned = pin(C_CPU);
    for (int i = 0; i < C_TRIES; i++) {
        struct ts req = { 0, (long)C_NS };
        uint64_t w0 = v->wall_time_ns;
        uint64_t t0 = mono();
        long rc = nsi(SYS_NANOSLEEP, (long)&req, 0, 0);
        uint64_t d = mono() - t0;
        uint64_t w1 = v->wall_time_ns;
        if (rc == 0 && d >= C_NS) {
            slept++;
            if (w1 == w0) early++;
        }
    }
    nsi(SYS_SCHED_CPU, 2, 0, 0);                    /* unpin */
    say("[sched] C early=%d of %d slept=%d\n", early, (long)C_TRIES, slept);
    say("PRADYOS_SCHED_C early_ok=%d pin_ok=%d\n", (long)(early >= C_MIN), pinned);
}

/* ---- arm M (DDR-1188 Amendment 3, T4): the one-shot budget ----
 * At most 1000 sleeps in a budget window end on a one-shot; the rest end on a
 * tick, and a second has 100 ticks. The first sleep is longer than a window
 * and takes one one-shot at its start, so the loop's first sleep opens a new
 * window and the whole second lies inside it. The lower bound keeps a kernel
 * with no one-shots at all (about 100 sleeps) from passing. */
#define M_NS     (50 * 1000ull)
#define M_MAX    1100
#define M_MIN    500

static void arm_m(void) {
    struct ts idle = { 1, (long)(50 * MS) };
    nsi(SYS_NANOSLEEP, (long)&idle, 0, 0);
    long n = 0;
    uint64_t t0 = mono();
    while (mono() - t0 < NS_PER_SEC) {
        struct ts req = { 0, (long)M_NS };
        if (nsi(SYS_NANOSLEEP, (long)&req, 0, 0) == 0) n++;
    }
    say("[sched] M sleeps=%d\n", n);
    say("PRADYOS_SCHED_M capped_ok=%d\n", (long)(n >= M_MIN && n <= M_MAX));
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    arm_b2();     /* first: it forks, so before arm A creates threads */
    arm_a();
    arm_b();
    arm_c();
    arm_m();
    say("PRADYOS_SCHED_DONE\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
