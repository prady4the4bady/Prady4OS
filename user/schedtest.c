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
 * C1.5 P3 arms (DDR-1188 Amendment 4): D (C4), E (C5), F (C6), G (C7),
 * H (C8, C8b; with user/schedagt.c), N (C14), P (C16, DDR-1193 D1).
 * C1.5 P4 arms (DDR-1188 Amendment 5): I (C9), J (C10), Q (C17).
 * C1.5 P5 arms (DDR-1188 Amendment 6): K (C11), K0 (C11b), O (C15).
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
 * Back-to-back sleeps that end on a tick are phase-locked to it: each starts
 * just after the tick that ended the one before, so each lasts about a whole
 * 10 ms tick, whichever CPU's tick ends it. A 2 ms sleep that ends under 4 ms
 * was therefore ended by a one-shot. `nowall` (the BSP's wall_time_ns did not
 * move across the sleep) is printed as data only: a tick on the sleeper's own
 * CPU can end a sleep without the BSP's clock moving, so it proves less. */
#define C_TRIES  50
#define C_NS     (2 * MS)
#define C_LATE   (4 * MS)
#define C_MIN    25
#define C_CPU    2

static void arm_c(void) {
    const struct vdso *v = (const struct vdso *)VDSO_USER_VA;
    long early = 0, slept = 0, nowall = 0;
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
            if (d < C_LATE) early++;
            if (w1 == w0) nowall++;
        }
    }
    nsi(SYS_SCHED_CPU, 2, 0, 0);                    /* unpin */
    say("[sched] C early=%d of %d slept=%d nowall=%d\n", early, (long)C_TRIES, slept, nowall);
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

/* ---- C1.5 P3 (DDR-1188 Amendment 4): FIFO, RR, throttle, RES_SCHED, PI ----
 * Every RT thread is pinned. The main thread pins itself to CPU 0 first, so it
 * never queues behind an RT spinner. Shared state lives in mapped pages (the
 * probe has no writable segment). */
#define POL_OTHER   0
#define POL_FIFO    1
#define POL_RR      2
#define EPERM       1
#define P3_CPU      2
#define P3_WCPU     3
#define FX_WAIT     0
#define FX_WAKE     1

static long setsched(long pol, long prio) {
    return nsi(SYS_SCHED_SETSCHED, 0, pol, prio);
}

static void fx_wait(volatile long *w, long val) {
    nsi6(SYS_FUTEX, (long)w, FX_WAIT, val, 0, 0, 0);
}

static void fx_wake(volatile long *w) {
    nsi6(SYS_FUTEX, (long)w, FX_WAKE, 1, 0, 0, 0);
}

/* run_ticks of `pid` (DDR-754), -1 if it is not found. *hint caches its
 * SYS_GETPROCS index, re-checked by pid on every call. */
static long rticks(long pid, long *hint) {
    struct { uint32_t pid, ppid, state, flags; char name[16];
             uint64_t run_ticks, dispatches; } pi;
    if (*hint >= 0 && nsi(SYS_GETPROCS, *hint, (long)&pi, 0) == 1 && (long)pi.pid == pid)
        return (long)pi.run_ticks;
    for (long i = 0; i < 4096 && nsi(SYS_GETPROCS, i, (long)&pi, 0) == 1; i++)
        if ((long)pi.pid == pid) {
            *hint = i;
            return (long)pi.run_ticks;
        }
    return -1;
}

static void *page(void) {
    long pg = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (pg < 0) fail("mmap");
    return (void *)pg;
}

/* Spin until *p == want; 0 after `sec` s of CLOCK_MONOTONIC. */
static int await_eq(volatile long *p, long want, long sec) {
    uint64_t end = mono() + (uint64_t)sec * NS_PER_SEC;
    while (__atomic_load_n(p, __ATOMIC_ACQUIRE) != want) {
        if (mono() > end) return 0;
        __asm__ volatile("pause");
    }
    return 1;
}

/* ---- arms D and N: wake latency, in the spinner's own ticks ----
 * The spinner resumes when the woken thread blocks again, reads its own
 * run_ticks (r0) and wakes it at once. The woken thread reads the spinner's
 * run_ticks when it runs (r1). With P3 the next tick preempts: r1 - r0 = 1.
 * Without, the spinner first spends the quantum it resumed with: 2. */
#define DN_TRIES  50
#define DN_LATE   5

struct dn_state {
    volatile long fw, armed, abort, done, spid, wpid;
    volatile long r0, late, max, pinned, setok;
    long spol, sprio, wprio;
};

__attribute__((noreturn, force_align_arg_pointer)) static void dn_woken(long arg) {
    struct dn_state *s = (struct dn_state *)arg;
    long hint = -1;
    __atomic_add_fetch(&s->pinned, pin(P3_CPU), __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&s->setok, setsched(POL_FIFO, s->wprio) == 0, __ATOMIC_ACQ_REL);
    for (long i = 0; i < DN_TRIES && !s->abort; i++) {
        __atomic_store_n(&s->armed, 2 * i + 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&s->fw, __ATOMIC_ACQUIRE) && !s->abort)
            fx_wait(&s->fw, 0);
        long d = rticks(s->spid, &hint) - s->r0;
        if (d >= 2) s->late++;
        if (d > s->max) s->max = d;
        s->fw = 0;
    }
    setsched(POL_OTHER, 0);
    __atomic_store_n(&s->done, 1, __ATOMIC_RELEASE);
    texit();
}

__attribute__((noreturn, force_align_arg_pointer)) static void dn_spinner(long arg) {
    struct dn_state *s = (struct dn_state *)arg;
    long hint = -1;
    __atomic_add_fetch(&s->pinned, pin(P3_CPU), __ATOMIC_ACQ_REL);
    if (s->spol != POL_OTHER)
        __atomic_add_fetch(&s->setok, setsched(s->spol, s->sprio) == 0, __ATOMIC_ACQ_REL);
    else
        __atomic_add_fetch(&s->setok, 1, __ATOMIC_ACQ_REL);
    while (!__atomic_load_n(&s->spid, __ATOMIC_ACQUIRE))   /* tid from spawn(); */
        __asm__ volatile("pause");                         /* SYS_GETPID is the tgid */
    for (long i = 0; i < DN_TRIES; i++) {
        if (!await_eq(&s->armed, 2 * i + 1, 3)) {
            s->abort = 1;                    /* never re-armed: the woken never ran */
            fx_wake(&s->fw);
            break;
        }
        s->r0 = rticks(s->spid, &hint);
        __atomic_store_n(&s->fw, 1, __ATOMIC_RELEASE);
        fx_wake(&s->fw);
    }
    setsched(POL_OTHER, 0);
    texit();
}

/* Returns 1 when the arm passes. */
static long arm_dn(const char *name, long spol, long sprio, long wprio) {
    struct dn_state *s = page();
    s->spol = spol; s->sprio = sprio; s->wprio = wprio; s->spid = 0;
    s->wpid = spawn(dn_woken, (long)s);
    if (!await_eq(&s->armed, 1, 5)) fail("DN arm");
    __atomic_store_n(&s->spid, spawn(dn_spinner, (long)s), __ATOMIC_RELEASE);
    uint64_t end = mono() + 30 * NS_PER_SEC;
    while (!__atomic_load_n(&s->done, __ATOMIC_ACQUIRE) && mono() < end)
        nsi(SYS_YIELD, 0, 0, 0);
    long ok = s->done && !s->abort && s->late <= DN_LATE && s->pinned == 2 && s->setok == 2;
    say("[sched] %s late=%d of %d max=%d abort=%d pinned=%d setok=%d\n", name,
        s->late, (long)DN_TRIES, s->max, s->abort, s->pinned, s->setok);
    return ok;
}

/* ---- arm E: two RR threads of one priority both make progress ----
 * Each thread takes RR BEFORE it pins, so the second reaches CPU 2 as an RR
 * thread and only rotation (not a throttle gap, which goes to SCHED_OTHER)
 * lets it run there. Without that order arm E depended on the throttle, and
 * mutant C7 went red here instead of on arm G. */
struct e_state { volatile long ready, go, done, n[2], pinned, setok; volatile uint64_t end; };

__attribute__((noreturn, force_align_arg_pointer)) static void e_thread(long arg) {
    long me = arg & 1;
    struct e_state *e = (struct e_state *)(arg & ~1L);
    __atomic_add_fetch(&e->setok, setsched(POL_RR, 10) == 0, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&e->pinned, pin(P3_CPU), __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&e->ready, 1, __ATOMIC_ACQ_REL);
    while (!__atomic_load_n(&e->go, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    long n = 0;
    while (mono() < e->end)
        n++;
    e->n[me] = n;
    setsched(POL_OTHER, 0);
    __atomic_add_fetch(&e->done, 1, __ATOMIC_ACQ_REL);
    texit();
}

static long arm_e(void) {
    struct e_state *e = page();
    spawn(e_thread, (long)e | 0);
    spawn(e_thread, (long)e | 1);
    long ready = await_eq(&e->ready, 2, 10);   /* 0: one never got CPU 2 */
    e->end = mono() + NS_PER_SEC;
    __atomic_store_n(&e->go, 1, __ATOMIC_RELEASE);
    await_eq(&e->done, 2, 20);
    long ok = ready && e->done == 2 && e->n[0] > 0 && e->n[1] > 0 &&
              e->pinned == 2 && e->setok == 2;
    say("[sched] E n0=%d n1=%d done=%d ready=%d\n", e->n[0], e->n[1], e->done, ready);
    return ok;
}

/* ---- arm F: equal FIFO threads do not rotate on quantum expiry ---- */
#define F_TICKS 30

struct f_state { volatile long fw, ready2, ran2, seen, done, pid1, pinned, setok, spun; };

__attribute__((noreturn, force_align_arg_pointer)) static void f_second(long arg) {
    struct f_state *f = (struct f_state *)arg;
    __atomic_add_fetch(&f->pinned, pin(P3_CPU), __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&f->setok, setsched(POL_FIFO, 10) == 0, __ATOMIC_ACQ_REL);
    __atomic_store_n(&f->ready2, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&f->fw, __ATOMIC_ACQUIRE))
        fx_wait(&f->fw, 0);
    __atomic_store_n(&f->ran2, 1, __ATOMIC_RELEASE);
    setsched(POL_OTHER, 0);
    __atomic_add_fetch(&f->done, 1, __ATOMIC_ACQ_REL);
    texit();
}

__attribute__((noreturn, force_align_arg_pointer)) static void f_first(long arg) {
    struct f_state *f = (struct f_state *)arg;
    long hint = -1;
    __atomic_add_fetch(&f->pinned, pin(P3_CPU), __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&f->setok, setsched(POL_FIFO, 10) == 0, __ATOMIC_ACQ_REL);
    while (!__atomic_load_n(&f->pid1, __ATOMIC_ACQUIRE))   /* tid from spawn() */
        __asm__ volatile("pause");
    long me = f->pid1;
    await_eq(&f->ready2, 1, 10);
    __atomic_store_n(&f->fw, 1, __ATOMIC_RELEASE);
    fx_wake(&f->fw);                     /* the second is READY behind us now */
    long t0 = rticks(me, &hint), t = t0;
    while (t >= 0 && t - t0 < F_TICKS && !__atomic_load_n(&f->ran2, __ATOMIC_ACQUIRE))
        t = rticks(me, &hint);
    f->spun = t - t0;
    f->seen = __atomic_load_n(&f->ran2, __ATOMIC_ACQUIRE);
    setsched(POL_OTHER, 0);              /* "blocks": steps out of the class */
    __atomic_add_fetch(&f->done, 1, __ATOMIC_ACQ_REL);
    texit();
}

static long arm_f(void) {
    struct f_state *f = page();
    spawn(f_second, (long)f);
    if (!await_eq(&f->ready2, 1, 10)) fail("F ready");
    __atomic_store_n(&f->pid1, spawn(f_first, (long)f), __ATOMIC_RELEASE);
    await_eq(&f->done, 2, 20);
    long ok = f->done == 2 && f->seen == 0 && f->ran2 && f->spun >= F_TICKS &&
              f->pinned == 2 && f->setok == 2;
    say("[sched] F seen=%d ran2=%d spun=%d done=%d\n", f->seen, f->ran2, f->spun, f->done);
    return ok;
}

/* ---- arm G: the throttle leaves SCHED_OTHER at least 4 ticks a second ---- */
#define G_WIN   5
#define G_MIN   4

struct g_state { volatile long stop, opid, ready, done, w[G_WIN], pinned, setok; };

__attribute__((noreturn, force_align_arg_pointer)) static void g_other(long arg) {
    struct g_state *g = (struct g_state *)arg;
    __atomic_add_fetch(&g->pinned, pin(P3_CPU), __ATOMIC_ACQ_REL);
    __atomic_store_n(&g->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g->stop, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    __atomic_add_fetch(&g->done, 1, __ATOMIC_ACQ_REL);
    texit();
}

__attribute__((noreturn, force_align_arg_pointer)) static void g_fifo(long arg) {
    struct g_state *g = (struct g_state *)arg;
    long hint = -1;
    __atomic_add_fetch(&g->pinned, pin(P3_CPU), __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&g->setok, setsched(POL_FIFO, 10) == 0, __ATOMIC_ACQ_REL);
    long prev = rticks(g->opid, &hint);
    for (int i = 0; i < G_WIN; i++) {
        uint64_t end = mono() + NS_PER_SEC;
        while (mono() < end)
            __asm__ volatile("pause");
        long now = rticks(g->opid, &hint);
        g->w[i] = now - prev;
        prev = now;
    }
    __atomic_store_n(&g->stop, 1, __ATOMIC_RELEASE);
    setsched(POL_OTHER, 0);
    __atomic_add_fetch(&g->done, 1, __ATOMIC_ACQ_REL);
    texit();
}

static long arm_g(void) {
    struct g_state *g = page();
    __atomic_store_n(&g->opid, spawn(g_other, (long)g), __ATOMIC_RELEASE);
    if (!await_eq(&g->ready, 1, 10)) fail("G ready");
    spawn(g_fifo, (long)g);
    await_eq(&g->done, 2, 30);
    long ok = g->done == 2 && g->pinned == 2 && g->setok == 1;
    long mn = 1L << 30;
    for (int i = 0; i < G_WIN; i++)
        if (g->w[i] < mn) mn = g->w[i];
    if (mn < G_MIN) ok = 0;
    say("[sched] G w=%d,%d,%d,%d,%d min=%d\n", g->w[0], g->w[1], g->w[2], g->w[3], g->w[4], mn);
    return ok;
}

/* ---- arm H: RES_SCHED (K1) ---- */
static long find_pid(const char *name) {
    struct { uint32_t pid, ppid, state, flags; char name[16];
             uint64_t run_ticks, dispatches; } pi;
    for (long i = 0; i < 4096 && nsi(SYS_GETPROCS, i, (long)&pi, 0) == 1; i++) {
        int k = 0;
        while (name[k] && pi.name[k] == name[k]) k++;
        if (!name[k] && !pi.name[k])
            return (long)pi.pid;
    }
    return -1;
}

static void arm_h(void) {
    long kid = nsi(SYS_FORK, 0, 0, 0);
    if (kid < 0) fail("H fork");
    if (kid == 0) {
        long rc = setsched(POL_FIFO, 10);
        nsi(SYS_EXIT, rc == -EPERM ? 0 : 1, 0, 0);
        for (;;) { }
    }
    int st = 0x7fffffff;
    long reaped = nsi(SYS_WAIT4, kid, (long)&st, 0);
    long fork_eperm = reaped == kid && st == 0;
    long agt = -1;
    uint64_t end = mono() + 10 * NS_PER_SEC;
    while ((agt = find_pid("SCHEDAGT")) < 0 && mono() < end)
        nsi(SYS_YIELD, 0, 0, 0);
    long grc = agt > 0 ? nsi(SYS_SCHED_GRANT, agt, 1L << 26, 0) : 0;
    long own = setsched(POL_FIFO, 10);   /* the probe itself holds RES_SCHED */
    setsched(POL_OTHER, 0);
    say("[sched] H fork_st=%d agent=%d grant_rc=%d own_rc=%d\n", (long)st, agt, grc, own);
    say("PRADYOS_SCHED_H fork_eperm=%d agent_grant_eperm=%d own_ok=%d\n",
        fork_eperm, (long)(grc == -EPERM), (long)(own == 0));
}

/* ---- arm P: kmutex priority inheritance (DDR-1193 D1) ----
 * A SCHED_OTHER holder on CPU 2 takes the test kmutex for 30 of its own ticks;
 * a FIFO-10 spinner then takes CPU 2; a FIFO-20 waiter on CPU 3 locks it. With
 * inheritance the holder runs at 20 and the wait is about 0.3 s; without it the
 * holder gets one throttle tick in 20, about 6 s. */
#define P_HOLD   30
#define P_MAX_MS 2000

struct p_state {
    volatile long held, spin_on, stop, done, wait_ms, hold_rc, wait_rc, pinned, setok;
};

__attribute__((noreturn, force_align_arg_pointer)) static void p_holder(long arg) {
    struct p_state *p = (struct p_state *)arg;
    __atomic_add_fetch(&p->pinned, pin(P3_CPU), __ATOMIC_ACQ_REL);
    __atomic_store_n(&p->held, 1, __ATOMIC_RELEASE);
    p->hold_rc = nsi(SYS_SCHED_CPU, 3, P_HOLD, 0);
    __atomic_add_fetch(&p->done, 1, __ATOMIC_ACQ_REL);
    texit();
}

__attribute__((noreturn, force_align_arg_pointer)) static void p_spinner(long arg) {
    struct p_state *p = (struct p_state *)arg;
    __atomic_add_fetch(&p->pinned, pin(P3_CPU), __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&p->setok, setsched(POL_FIFO, 10) == 0, __ATOMIC_ACQ_REL);
    __atomic_store_n(&p->spin_on, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&p->stop, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    setsched(POL_OTHER, 0);
    __atomic_add_fetch(&p->done, 1, __ATOMIC_ACQ_REL);
    texit();
}

__attribute__((noreturn, force_align_arg_pointer)) static void p_waiter(long arg) {
    struct p_state *p = (struct p_state *)arg;
    __atomic_add_fetch(&p->pinned, pin(P3_WCPU), __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&p->setok, setsched(POL_FIFO, 20) == 0, __ATOMIC_ACQ_REL);
    uint64_t t0 = mono();
    p->wait_rc = nsi(SYS_SCHED_CPU, 4, 0, 0);
    p->wait_ms = (long)((mono() - t0) / MS);
    setsched(POL_OTHER, 0);
    __atomic_store_n(&p->stop, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&p->done, 1, __ATOMIC_ACQ_REL);
    texit();
}

static long arm_p(void) {
    struct p_state *p = page();
    spawn(p_holder, (long)p);
    if (!await_eq(&p->held, 1, 10)) fail("P held");
    struct ts gap = { 0, (long)(30 * MS) };
    nsi(SYS_NANOSLEEP, (long)&gap, 0, 0);           /* the holder is inside its hold */
    spawn(p_spinner, (long)p);
    if (!await_eq(&p->spin_on, 1, 10)) fail("P spin");
    spawn(p_waiter, (long)p);
    await_eq(&p->done, 3, 30);
    long ok = p->done == 3 && p->hold_rc == 0 && p->wait_rc == 0 &&
              p->wait_ms < P_MAX_MS && p->pinned == 3 && p->setok == 2;
    say("[sched] P wait_ms=%d hold_rc=%d wait_rc=%d done=%d\n",
        p->wait_ms, p->hold_rc, p->wait_rc, p->done);
    return ok;
}

static void arms_p3(void) {
    long main_pin = pin(0);
    long d = arm_dn("D", POL_OTHER, 0, 10);
    say("PRADYOS_SCHED_D preempt_ok=%d\n", d);
    long n = arm_dn("N", POL_FIFO, 10, 20);
    say("PRADYOS_SCHED_N prio_ok=%d\n", n);
    say("PRADYOS_SCHED_E rr_ok=%d\n", arm_e());
    say("PRADYOS_SCHED_F fifo_ok=%d\n", arm_f());
    say("PRADYOS_SCHED_G throttle_ok=%d\n", arm_g());
    say("PRADYOS_SCHED_P pi_ok=%d main_pin=%d\n", arm_p(), main_pin);
    nsi(SYS_SCHED_CPU, 2, 0, 0);
}

/* ---- C1.5 P4 (DDR-1188 Amendment 5): SCHED_DEADLINE, arms I, J, Q ----
 * Every deadline thread pins to P4_CPU first (D2). Each arm returns its
 * threads to SCHED_OTHER before they exit, so the next arm starts with no
 * admitted bandwidth on that CPU. */
#define POL_DL      3
#define EBUSY       16
#define P4_CPU      2

static long setdl(long rt_ms, long per_ms) {
    return nsi6(SYS_SCHED_SETSCHED, 0, POL_DL, 0, rt_ms * (long)MS, per_ms * (long)MS, 0);
}

static void fx_wake_all(volatile long *w) {
    nsi6(SYS_FUTEX, (long)w, FX_WAKE, 64, 0, 0, 0);
}

/* ---- arm I (C9): admission refuses a CPU over 0.90 ----
 * Steps, one per value of `go`: 1 T0 asks 50/100; 2 T1 asks 30/100; 3 T2 asks
 * 20/100 (sum 1.00: -EBUSY); 4 T0 returns to OTHER; 5 T2 asks 20/100 again. */
#define I_END 6

struct i_state { volatile long go, ack, ready, done, pinned, rc[5]; };

__attribute__((noreturn, force_align_arg_pointer)) static void i_thread(long arg) {
    long k = arg & 3;
    struct i_state *s = (struct i_state *)(arg & ~3L);
    __atomic_add_fetch(&s->pinned, pin(P4_CPU), __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&s->ready, 1, __ATOMIC_ACQ_REL);
    long seen = 0;
    for (;;) {
        long g = __atomic_load_n(&s->go, __ATOMIC_ACQUIRE);
        if (g == seen) {
            fx_wait(&s->go, g);
            continue;
        }
        seen = g;
        if (g >= I_END)
            break;
        if (k == 0 && g == 1) s->rc[0] = setdl(50, 100);
        if (k == 1 && g == 2) s->rc[1] = setdl(30, 100);
        if (k == 2 && g == 3) s->rc[2] = setdl(20, 100);
        if (k == 0 && g == 4) s->rc[3] = setsched(POL_OTHER, 0);
        if (k == 2 && g == 5) s->rc[4] = setdl(20, 100);
        __atomic_add_fetch(&s->ack, 1, __ATOMIC_ACQ_REL);
    }
    setsched(POL_OTHER, 0);
    __atomic_add_fetch(&s->done, 1, __ATOMIC_ACQ_REL);
    texit();
}

static long arm_i(void) {
    struct i_state *s = page();
    for (long k = 0; k < 3; k++)
        spawn(i_thread, (long)s | k);
    if (!await_eq(&s->ready, 3, 10)) fail("I ready");
    long steps = 0;
    for (long g = 1; g < I_END; g++) {
        __atomic_store_n(&s->go, g, __ATOMIC_RELEASE);
        fx_wake_all(&s->go);
        if (!await_eq(&s->ack, 3 * g, 5))
            break;
        steps = g;
    }
    __atomic_store_n(&s->go, I_END, __ATOMIC_RELEASE);
    fx_wake_all(&s->go);
    await_eq(&s->done, 3, 10);
    long ok = steps == I_END - 1 && s->done == 3 && s->pinned == 3 &&
              s->rc[0] == 0 && s->rc[1] == 0 && s->rc[2] == -EBUSY &&
              s->rc[3] == 0 && s->rc[4] == 0;
    say("[sched] I rc=%d,%d,%d,%d,%d steps=%d done=%d\n", s->rc[0], s->rc[1], s->rc[2],
        s->rc[3], s->rc[4], steps, s->done);
    return ok;
}

/* ---- arm J (C10): EDF picks the earlier deadline ----
 * A (10 ms / 30 ms) and B (10 ms / 60 ms) wait on futexes. A SCHED_OTHER
 * spinner pinned to the same CPU wakes B, then A, and spins: with no IPI both
 * are READY when the next tick preempts it, and EDF must pick A. Releases are
 * 70 ms apart, so each wake starts from a fresh deadline (D3). Misses are data
 * (Amendment 5 D7), not asserted. */
#define J_TRIES 100
#define J_MIN   95

struct j_state {
    volatile long f[2], order, afirst, ran, ready, pinned, setok, done, stop, abort, trials;
    volatile long first, tries, spid;   /* first: which worker ran first this release */
    volatile long miss[2];
    volatile uint64_t rel;
};

__attribute__((noreturn, force_align_arg_pointer)) static void j_dl(long arg) {
    long k = arg & 1;                       /* 0 = A, 1 = B */
    struct j_state *s = (struct j_state *)(arg & ~1L);
    uint64_t per = (k ? 60 : 30) * MS;
    __atomic_add_fetch(&s->pinned, pin(P4_CPU), __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&s->setok, setdl(10, k ? 60 : 30) == 0, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&s->ready, 1, __ATOMIC_ACQ_REL);
    for (;;) {
        /* f[k]: 0 idle, 1 a release, 2 shut down. A release is consumed by
         * 1 -> 0 compare-and-exchange, so it can never overwrite the
         * terminal 2 the spinner publishes (CodeRabbit on #140). */
        long one = 1;
        while (__atomic_load_n(&s->f[k], __ATOMIC_ACQUIRE) == 0)
            fx_wait(&s->f[k], 0);
        if (!__atomic_compare_exchange_n(&s->f[k], &one, 0, 0, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
            break;                          /* it was 2: shut down */
        if (__atomic_fetch_add(&s->order, 1, __ATOMIC_ACQ_REL) == 0)
            s->first = k;               /* scored by the spinner, if the release counts */
        if (mono() - s->rel > per)
            s->miss[k]++;
        __atomic_add_fetch(&s->ran, 1, __ATOMIC_ACQ_REL);
    }
    setsched(POL_OTHER, 0);
    __atomic_add_fetch(&s->done, 1, __ATOMIC_ACQ_REL);
    texit();
}

__attribute__((noreturn, force_align_arg_pointer)) static void j_spin(long arg) {
    struct j_state *s = (struct j_state *)arg;
    __atomic_add_fetch(&s->pinned, pin(P4_CPU), __ATOMIC_ACQ_REL);
    if (!await_eq(&s->ready, 2, 10))
        s->abort = 1;
    while (!__atomic_load_n(&s->spid, __ATOMIC_ACQUIRE))   /* own tid, from spawn() */
        __asm__ volatile("pause");
    long hint = -1;
    /* A release is scored only when both FUTEX_WAKEs woke a parked worker
     * (each returned 1) and no tick landed between them: the spinner's own
     * run_ticks did not move across the two wakes. Anything else is retried
     * unscored (CodeRabbit on #140), so B cannot run alone and be counted. */
    while (s->trials < J_TRIES && s->tries < 3 * J_TRIES && !s->abort) {
        s->tries++;
        /* 70 ms of quiet before each release: both workers have parked, and
         * both previous deadlines have passed. */
        struct ts req = { 0, (long)(70 * MS) };
        nsi(SYS_NANOSLEEP, (long)&req, 0, 0);
        s->order = 0;
        s->ran = 0;
        s->first = -1;
        s->rel = mono();
        long t0 = rticks(s->spid, &hint);
        __atomic_store_n(&s->f[1], 1, __ATOMIC_RELEASE);
        long wb = nsi6(SYS_FUTEX, (long)&s->f[1], FX_WAKE, 1, 0, 0, 0);   /* B first */
        __atomic_store_n(&s->f[0], 1, __ATOMIC_RELEASE);
        long wa = nsi6(SYS_FUTEX, (long)&s->f[0], FX_WAKE, 1, 0, 0, 0);   /* then A */
        long t1 = rticks(s->spid, &hint);
        if (!await_eq(&s->ran, 2, 3)) {
            s->abort = 1;
            break;
        }
        if (wb == 1 && wa == 1 && t0 >= 0 && t1 == t0) {
            s->trials++;
            if (s->first == 0)
                s->afirst++;
        }
    }
    __atomic_store_n(&s->stop, 1, __ATOMIC_RELEASE);
    /* The terminal value on the futex words: a worker that has not reached
     * FUTEX_WAIT sees a value mismatch instead of sleeping (CodeRabbit). */
    __atomic_store_n(&s->f[0], 2, __ATOMIC_RELEASE);
    __atomic_store_n(&s->f[1], 2, __ATOMIC_RELEASE);
    fx_wake(&s->f[0]);
    fx_wake(&s->f[1]);
    __atomic_add_fetch(&s->done, 1, __ATOMIC_ACQ_REL);
    texit();
}

static long arm_j(void) {
    struct j_state *s = page();
    spawn(j_dl, (long)s | 0);
    spawn(j_dl, (long)s | 1);
    __atomic_store_n(&s->spid, spawn(j_spin, (long)s), __ATOMIC_RELEASE);
    await_eq(&s->done, 3, 90);
    long ok = s->done == 3 && !s->abort && s->trials == J_TRIES && s->afirst >= J_MIN &&
              s->pinned == 3 && s->setok == 2;
    say("[sched] J afirst=%d of %d tries=%d miss_a=%d miss_b=%d abort=%d setok=%d\n",
        s->afirst, s->trials, s->tries, s->miss[0], s->miss[1], s->abort, s->setok);
    return ok;
}

/* ---- arm Q (C17): the CBS budget bounds a deadline spinner ----
 * A deadline spinner (10 ms / 100 ms) and a SCHED_OTHER thread share P4_CPU.
 * With the budget charged, the spinner runs about one tick in ten, so the
 * other thread gets most of each second; uncharged, only K2's gaps. */
#define Q_WIN 3
#define Q_MIN 30

struct q_state { volatile long stop, opid, ready, done, w[Q_WIN], pinned, setok; };

__attribute__((noreturn, force_align_arg_pointer)) static void q_other(long arg) {
    struct q_state *q = (struct q_state *)arg;
    __atomic_add_fetch(&q->pinned, pin(P4_CPU), __ATOMIC_ACQ_REL);
    __atomic_store_n(&q->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&q->stop, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    __atomic_add_fetch(&q->done, 1, __ATOMIC_ACQ_REL);
    texit();
}

__attribute__((noreturn, force_align_arg_pointer)) static void q_dl(long arg) {
    struct q_state *q = (struct q_state *)arg;
    long hint = -1;
    __atomic_add_fetch(&q->pinned, pin(P4_CPU), __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&q->setok, setdl(10, 100) == 0, __ATOMIC_ACQ_REL);
    long prev = rticks(q->opid, &hint);
    for (int i = 0; i < Q_WIN; i++) {
        uint64_t end = mono() + NS_PER_SEC;
        while (mono() < end)
            __asm__ volatile("pause");
        long now = rticks(q->opid, &hint);
        q->w[i] = now - prev;
        prev = now;
    }
    __atomic_store_n(&q->stop, 1, __ATOMIC_RELEASE);
    setsched(POL_OTHER, 0);
    __atomic_add_fetch(&q->done, 1, __ATOMIC_ACQ_REL);
    texit();
}

static long arm_q(void) {
    struct q_state *q = page();
    __atomic_store_n(&q->opid, spawn(q_other, (long)q), __ATOMIC_RELEASE);
    if (!await_eq(&q->ready, 1, 10)) fail("Q ready");
    spawn(q_dl, (long)q);
    await_eq(&q->done, 2, 30);
    long ok = q->done == 2 && q->pinned == 2 && q->setok == 1;
    long mn = 1L << 30;
    for (int i = 0; i < Q_WIN; i++)
        if (q->w[i] < mn) mn = q->w[i];
    if (mn < Q_MIN) ok = 0;
    say("[sched] Q w=%d,%d,%d min=%d setok=%d\n", q->w[0], q->w[1], q->w[2], mn, q->setok);
    return ok;
}

static void arms_p4(void) {
    pin(0);
    say("PRADYOS_SCHED_I adm_ok=%d\n", arm_i());
    say("PRADYOS_SCHED_J edf_ok=%d\n", arm_j());
    say("PRADYOS_SCHED_Q cbs_ok=%d\n", arm_q());
    nsi(SYS_SCHED_CPU, 2, 0, 0);
}

/* ---- C1.5 P5 (DDR-1188 Amendment 6): pressure stall information ----
 * SYS_PSI(res, cpu) returns the stalled ticks of the last 100: res 0 is a
 * CPU's queue holding a READY thread, res 1 a thread waiting for a disk read. */
#define PSI_CPU   0
#define PSI_IO    1
#define K_MIN     50
#define K0_MAX    5

static long psi(long res, long cpu) {
    return nsi(SYS_PSI, res, cpu, 0);
}

static void sleep_ms(long ms) {
    struct ts req = { ms / 1000, (ms % 1000) * (long)MS };
    nsi(SYS_NANOSLEEP, (long)&req, 0, 0);
}

/* ---- arms K (C11) and K0 (C11b): CPU pressure on CPU 2 ---- */
struct k_state { volatile long stop, ready, done, pinned; };

__attribute__((noreturn, force_align_arg_pointer)) static void k_spin(long arg) {
    struct k_state *k = (struct k_state *)arg;
    __atomic_add_fetch(&k->pinned, pin(P4_CPU), __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&k->ready, 1, __ATOMIC_ACQ_REL);
    while (!__atomic_load_n(&k->stop, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    __atomic_add_fetch(&k->done, 1, __ATOMIC_ACQ_REL);
    texit();
}

static void arms_k(long *k_ok, long *k0_ok) {
    struct k_state *k = page();
    say("[sched] K base=%d,%d,%d,%d\n", psi(PSI_CPU, 0), psi(PSI_CPU, 1), psi(PSI_CPU, 2),
        psi(PSI_CPU, 3));                   /* data: every CPU before the load */
    spawn(k_spin, (long)k);
    spawn(k_spin, (long)k);
    long ready = await_eq(&k->ready, 2, 10);
    sleep_ms(1500);                         /* a full window with both spinning */
    long load = psi(PSI_CPU, P4_CPU);
    __atomic_store_n(&k->stop, 1, __ATOMIC_RELEASE);
    long done = await_eq(&k->done, 2, 10);
    sleep_ms(1500);                         /* the window has forgotten them */
    long idle = psi(PSI_CPU, P4_CPU);
    *k_ok = ready && k->pinned == 2 && load >= K_MIN;
    *k0_ok = done && load >= 0 && idle >= 0 && idle <= K0_MAX;
    say("[sched] K load=%d idle=%d pinned=%d done=%d\n", load, idle, k->pinned, done);
}

/* ---- arm O (C15): I/O pressure during a 16 MiB disk read ---- */
struct o_state { volatile long done, rc; };

__attribute__((noreturn, force_align_arg_pointer)) static void o_reader(long arg) {
    struct o_state *o = (struct o_state *)arg;
    o->rc = nsi(SYS_SCHED_CPU, 5, 16, 0);   /* probe op 5: read 16 MiB */
    __atomic_store_n(&o->done, 1, __ATOMIC_RELEASE);
    texit();
}

static long arm_o(void) {
    struct o_state *o = page();
    spawn(o_reader, (long)o);
    long peak = 0, polls = 0;
    uint64_t end = mono() + 30 * NS_PER_SEC;
    while (!__atomic_load_n(&o->done, __ATOMIC_ACQUIRE) && mono() < end) {
        long v = psi(PSI_IO, 0);
        if (v > peak) peak = v;
        polls++;
        sleep_ms(10);
    }
    long v = psi(PSI_IO, 0);                /* the last ticks of the read */
    if (v > peak) peak = v;
    long done = o->done;
    sleep_ms(2000);                         /* §4: back to 0 within 2 s */
    long after = psi(PSI_IO, 0);
    say("[sched] O peak=%d after=%d rc=%d polls=%d done=%d\n", peak, after, o->rc, polls, done);
    return done && o->rc == 0 && peak > 0 && after == 0;
}

static void arms_p5(void) {
    pin(0);
    long k_ok = 0, k0_ok = 0;
    arms_k(&k_ok, &k0_ok);
    say("PRADYOS_SCHED_K some_ok=%d\n", k_ok);
    say("PRADYOS_SCHED_K0 idle_ok=%d\n", k0_ok);
    say("PRADYOS_SCHED_O io_ok=%d\n", arm_o());
    nsi(SYS_SCHED_CPU, 2, 0, 0);
}

/* ---- C1.5 P6 (DDR-1188 Amendment 7): tickless idle on the APs ----
 * Probe-only ops of SYS_SCHED_CPU: 6 (cpu, on) isolates a CPU so that only
 * threads pinned to it run there; 7 (cpu) reads its timer-interrupt count;
 * 8 runs one synchronize_rcu and returns how long it took, in us. */
#define L_CPU      3
#define L_MAX      5        /* timer interrupts in 1 s on an idle isolated CPU */
#define L1_N       50
/* 20.1 ms, not 20: nanosleep's tick backstop (rem / 10 ms + 1 ticks, expired by
 * the BSP's sweep) then lands after the deadline, never before it. With a whole
 * number of ticks it landed at or before the deadline, its wake ended any stop
 * early and the exit's one-shot gave the precise wake whether or not the stop
 * counted to the deadline (C12b survived on d83db5b, DDR-1188 A7). */
#define L1_NS      (20 * MS + 100000ull)
#define L1_FINE    (2 * MS)         /* a wake this close is not a tick's */
#define L1_FINE_N  40               /* of L1_N; C12b leaves about 9.9 ms late */
#define L1_LATE    (2 * 10 * MS)    /* 2 ticks */
#define L2_N       20
#define L2_MAX_US  50000

struct l1_state { volatile long done, pinned, valid, fine; volatile uint64_t late; };

__attribute__((noreturn, force_align_arg_pointer)) static void l1_sleeper(long arg) {
    struct l1_state *s = (struct l1_state *)arg;
    s->pinned = pin(L_CPU);
    uint64_t worst = 0;
    for (int i = 0; i < L1_N && s->pinned; i++) {
        struct ts req = { 0, (long)L1_NS };
        uint64_t t0 = mono();
        long r = nsi(SYS_NANOSLEEP, (long)&req, 0, 0);
        uint64_t d = mono() - t0, want = L1_NS;
        if (r != 0 || d < want)
            continue;                 /* a failed or short sleep is not a sample */
        s->valid++;
        if (d - want < L1_FINE) s->fine++;
        if (d - want > worst) worst = d - want;
    }
    s->late = worst;
    __atomic_store_n(&s->done, 1, __ATOMIC_RELEASE);
    texit();
}

static void arms_p6(void) {
    pin(0);
    long iso = nsi6(SYS_SCHED_CPU, 6, L_CPU, 1, 0, 0, 0);
    sleep_ms(1000);                          /* drain: what was there finishes */
    long i0 = nsi(SYS_SCHED_CPU, 7, L_CPU, 0);
    long s0 = nsi(SYS_SCHED_CPU, 7, L_CPU, 1), r0 = nsi(SYS_SCHED_CPU, 7, L_CPU, 2);
    long w0 = nsi(SYS_SCHED_CPU, 7, L_CPU, 3);
    sleep_ms(1000);
    long i1 = nsi(SYS_SCHED_CPU, 7, L_CPU, 0);
    long s1 = nsi(SYS_SCHED_CPU, 7, L_CPU, 1), r1 = nsi(SYS_SCHED_CPU, 7, L_CPU, 2);
    long w1 = nsi(SYS_SCHED_CPU, 7, L_CPU, 3);
    long irqs = (i0 >= 0 && i1 >= 0) ? i1 - i0 : -1;
    say("[sched] L iso=%d irqs=%d stops=%d refused=%d found_work=%d\n", iso, irqs,
        s1 - s0, r1 - r0, w1 - w0);
    say("PRADYOS_SCHED_L nohz_ok=%d\n", (long)(iso == 0 && irqs >= 0 && irqs < L_MAX));

    struct l1_state *s = page();
    spawn(l1_sleeper, (long)s);
    long done = await_eq(&s->done, 1, 10);
    say("[sched] L1 late_us=%d fine=%d valid=%d pinned=%d done=%d\n", (long)(s->late / 1000),
        s->fine, s->valid, s->pinned, done);
    say("PRADYOS_SCHED_L1 wake_ok=%d\n",
        (long)(iso == 0 && done && s->pinned && s->valid == L1_N && s->fine >= L1_FINE_N &&
               s->late <= L1_LATE));

    sleep_ms(200);                           /* CPU 3 isolated and idle again */
    long worst = -1, bad = 0;
    for (int i = 0; i < L2_N; i++) {
        long us = nsi(SYS_SCHED_CPU, 8, 0, 0);
        if (us < 0) bad++;
        else if (us > worst) worst = us;
    }
    say("[sched] L2 worst_us=%d bad=%d\n", worst, bad);
    say("PRADYOS_SCHED_L2 eqs_ok=%d\n",
        (long)(iso == 0 && !bad && worst >= 0 && worst <= L2_MAX_US));
    nsi6(SYS_SCHED_CPU, 6, L_CPU, 0, 0, 0, 0);
    nsi(SYS_SCHED_CPU, 2, 0, 0);
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    arm_b2();     /* first: it forks, so before arm A creates threads */
    arm_h();      /* forks too; and SCHEDAGT is certainly still alive */
    arm_a();
    arm_b();
    arm_c();
    arm_m();
    arms_p3();
    arms_p4();
    arms_p5();
    arms_p6();
    say("PRADYOS_SCHED_DONE\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
