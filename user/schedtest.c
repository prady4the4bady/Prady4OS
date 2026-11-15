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
#define MAP_PRIVATE    0x02
#define MAP_ANONYMOUS  0x20
#define EINVAL         22
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

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
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

static struct {
    volatile long reads[2], back[2], pinned[2], done[2];
    volatile long ready;                 /* threads that finished phase 1   */
    volatile uint64_t seq;               /* ping-pong: odd = 0->1, even = 1->0 */
    volatile uint64_t val;               /* the published reading           */
    volatile long xback, xrounds, xtimeout;
} A;

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

/* Spin until A.seq == want; 0 after 5 s of CLOCK_MONOTONIC. */
static int await_seq(uint64_t want) {
    uint64_t end = mono() + 5 * NS_PER_SEC;
    while (__atomic_load_n(&A.seq, __ATOMIC_ACQUIRE) != want) {
        if (mono() > end) return 0;
        __asm__ volatile("pause");
    }
    return 1;
}

__attribute__((noreturn, force_align_arg_pointer)) static void a_thread(long me) {
    A.pinned[me] = pin(me);
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
    A.reads[me] = reads;
    A.back[me] = back;
    __atomic_add_fetch(&A.ready, 1, __ATOMIC_SEQ_CST);
    uint64_t bar = mono() + 10 * NS_PER_SEC;           /* both reach phase 2 */
    while (__atomic_load_n(&A.ready, __ATOMIC_SEQ_CST) < 2 && mono() < bar)
        __asm__ volatile("pause");
    /* Phase 2: cross-CPU ping-pong. Round r (1..2*X_ROUNDS): odd r is
     * 0 -> 1, even r is 1 -> 0. The sender publishes val, then seq = r; the
     * receiver waits for seq == r, reads its own clock, compares. */
    for (uint64_t r = 1; r <= 2 * X_ROUNDS; r++) {
        long sender = (r & 1) ? 0 : 1;
        if (me == sender) {
            if (!await_seq(r - 1)) { A.xtimeout = 1; break; }
            A.val = mono();
            __atomic_store_n(&A.seq, r, __ATOMIC_RELEASE);
        } else {
            if (!await_seq(r)) { A.xtimeout = 1; break; }
            uint64_t got = A.val;
            uint64_t mine = mono();
            if (mine < got) __atomic_add_fetch(&A.xback, 1, __ATOMIC_SEQ_CST);
            __atomic_add_fetch(&A.xrounds, 1, __ATOMIC_SEQ_CST);
        }
    }
    nsi(SYS_SCHED_CPU, 2, 0, 0);
    A.done[me] = 1;
    texit();
}

static void arm_a(void) {
    spawn(a_thread, 0);
    spawn(a_thread, 1);
    uint64_t end = mono() + 60 * NS_PER_SEC;
    while (!(A.done[0] && A.done[1])) {
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
    long pin_ok = A.pinned[0] && A.pinned[1];
    long xrounds = A.xtimeout ? 0 : A.xrounds;
    say("[sched] A reads0=%d reads1=%d back0=%d back1=%d xrounds=%d xback=%d rt_sec=%d\n",
        A.reads[0], A.reads[1], A.back[0], A.back[1], xrounds, A.xback, rt.sec);
    if (xrounds != 2 * X_ROUNDS) fail("A cross-CPU rounds incomplete");
    say("PRADYOS_SCHED_A back=%d xback=%d sys_ok=%d pin_ok=%d rt_ok=%d\n",
        A.back[0] + A.back[1], A.xback, sys_ok, pin_ok, rt_ok);
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

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    arm_a();
    arm_b();
    say("PRADYOS_SCHED_DONE\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
