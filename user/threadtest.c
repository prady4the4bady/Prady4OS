/* user/threadtest.c — ring-3 thread probe for smoke-thread (DDR-1181 §4).
 *
 * FAT-rooted, opt-in (QEMU_PROBES=thread), run with 4 vCPUs. Arms:
 *   A  a store by a thread is seen by its creator; clear_tid is zeroed at exit
 *   B  two threads, two TLS blocks: each reads its own %fs:0 self-pointer
 *   C  72 create/exit cycles leak no physical pages
 *   P  DDR-1186: a thread pinned to a CPU runs there; bad arguments refused
 *   D  DDR-1177 arm D: mprotect(PROT_READ) while a thread on another CPU writes
 *   E  DDR-1177 arm E: munmap while a thread on another CPU reads
 *      (D, E, M and G place their two threads with SYS_SCHED_CPU: without it
 *      the scheduler never spreads them under load, DDR-1181 §4.3)
 *   M  DDR-1177 M4: no read through a stale entry ever returns a freed frame's
 *      poison (KASAN fills freed frames with 0xDEADBEEFDEADBEEF)
 *   F  a fault in one thread ends the whole process
 *   G  DDR-1181 §3.7: a kernel copyout racing munmap of its buffer is
 *      -EFAULT, never a kernel panic
 *   FA-FJ  DDR-1181 Amendment 1 (P2): futex wait/wake, keys, timeout,
 *      signals and the CLEARTID wake (table in the amendment)
 *   SA SW SB SC SH SD  DDR-1193 (C1.4 P3): psync.h mutex, semaphore,
 *      condition variable and barrier, each arm in a forked child
 *
 * D, E/M, F and G each run in a forked CHILD (the test process). The child's
 * threads report through a MAP_SHARED anonymous page (C1.3 P1) and the parent
 * judges how the child ended. Every parent-side wait is bounded by the vDSO
 * clock; on expiry the parent SIGKILLs the child and reports what it saw -- a
 * mutant that leaves a stale TLB entry also leaves the child alive.
 *
 * The "after" counts use one shape: `f = flag; access; if (f) count++`. The
 * flag is set only after the operation RETURNED, so a count can only come from
 * an access that began after it.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */
#include "pradyos.h"
#include "pagemeter.h"   /* DDR-1185 */
#define PSYNC_STATS      /* DDR-1193: pmutex.waits, the SA contention count */
#include "psync.h"

#define nsi pradyos_syscall

#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_ANONYMOUS  0x20
#define WNOHANG        1
#define SIGKILL        9
#define SIGUSR1        10
#define EFAULT         14
#define FUTEX_WAIT     0
#define FUTEX_WAKE     1
#define FUTEX_PRIVATE  128
#define EINVAL         22

#define VDSO_USER_VA   0x00007FFFFFF00000ull
#define MS             1000000ull
#define STK            16384
#define PAT            0x5EED5EED5EED5EEDull
#define TRIALS         16
#define PLACED_MIN     12    /* DDR-1186 §4: concurrency-proven trials needed */
#define CPU_OP         1     /* DDR-1186: the operating thread's pin CPU      */
#define CPU_LOOP       2     /* DDR-1186: the sibling's pin CPU (never the BSP) */

/* SYS_MMAP and SYS_THREAD_CREATE take more than three arguments. */
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

static uint64_t now(void) { return *(volatile uint64_t *)VDSO_USER_VA; }

/* DDR-1195 §2: ring 3 may read the TSC (CR4.TSD clear; user/benchtest.c). */
static inline uint64_t tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

/* Spin to the next vDSO tick edge and return the TSC read just after it. */
static uint64_t tsc_at_edge(void) {
    uint64_t e = now();
    while (now() == e) __asm__ volatile("pause");
    return tsc();
}

/* Every line leaves in ONE write (DDR-1182 §3.1: a line written in pieces
 * can be split by other output). The line is built on the stack because a
 * probe may not carry a writable section (DDR-826). Understands %d (a long)
 * and %s only. */
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

/* One sentinel line, built locally and emitted by ONE write: a kernel line (a
 * child's "[trap] ... killing process") can otherwise land between the parts
 * of a line written piecewise, and the gate sees neither half (CI run
 * 39504298130, arm F: "group_killed=[trap] ..."). */
static void line1(const char *tag, long v) {
    char b[96];
    long n = 0;
    while (tag[n] && n < 60) { b[n] = tag[n]; n++; }
    char d[20];
    int nd = 0;
    unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
    do { d[nd++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) b[n++] = '-';
    while (nd) b[n++] = d[--nd];
    b[n++] = '\n';
    nsi(SYS_WRITE, 1, (long)b, n);
}

__attribute__((noreturn)) static void fail(const char *why) {
    say("THREAD FAIL: %s\n", why);
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

static long map(long len, int flags) {
    long p = nsi6(SYS_MMAP, 0, len, PROT_READ | PROT_WRITE, flags | MAP_ANONYMOUS, -1, 0);
    if (p < 0) fail("mmap");
    return p;
}

/* Results shared between a test child (all its threads) and the parent. */
struct shared {
    volatile long go;          /* parent/child -> threads: start             */
    volatile long done;        /* set AFTER the operation returned           */
    volatile long count;       /* loop iterations (progress)                 */
    volatile long late;        /* accesses that began after `done` and won   */
    volatile long poison;      /* reads that returned a value other than PAT */
    volatile long efault;      /* arm G: copyouts that returned -EFAULT      */
    volatile long gdone;       /* arm G: the copy thread finished            */
    volatile uint64_t page;    /* the page under test                        */
    volatile uint64_t tls[3][2];   /* arm B: two thread TLS blocks + main's */
    volatile long tls_ok;      /* arm B */
    volatile long a_store;     /* arm A (unused since DDR-1181 §4.1) */
    volatile uint64_t a_page;  /* arm A: a MAP_PRIVATE page of the creator */
    volatile long midcopy;     /* arm G: -EFAULTs that left bytes written     */
    volatile long lcpu;        /* D/E: the loop thread's CPU (DDR-1181 §4.3)   */
    volatile long ccpu;        /* D/E: the operating thread's CPU, at the op   */
    volatile long placed;      /* D/E/M: the trial proved the sibling ran in
                                * parallel (DDR-1186 §4)                    */
    volatile unsigned a_ctid;  /* arm A: zeroed by the kernel at thread exit */
    volatile unsigned c_ctid;  /* arm C */
    volatile uint32_t fw, fdw, few, ffw, fhw, fbw, fcw, ictid;   /* futex words */
    volatile long fq;          /* FD: threads about to wait               */
    volatile long frc, brc, crc, cms, erc, hrc;   /* futex return values  */
    volatile long ready, hdone;
    volatile long frc_f;       /* DDR-1195 §3: FF/FG WAIT result, 99 = not returned */
    volatile long ctsc;        /* DDR-1195 §2: FC TSC counts per ms */
};

static long spawn(void (*fn)(long), long arg, uint64_t tls, uint64_t ctid) {
    long stk = map(STK, MAP_PRIVATE);
    long tid = nsi6(SYS_THREAD_CREATE, (long)fn, stk + STK, arg, (long)tls, (long)ctid, 0);
    if (tid < 0) fail("create");
    return stk;
}

/* THREADTEST tcbs in the scheduler other than the caller, and how many of
 * them are zombies (SYS_GETPROCS walks the whole ring, zombies included). */
static void thr_left(long *left, long *zomb) {
    struct { uint32_t pid, ppid, state, flags; char name[16];
             uint64_t run_ticks, dispatches; } pi;
    long me = nsi(SYS_GETPID, 0, 0, 0);
    *left = *zomb = 0;
    for (long i = 0; i < 4096 && nsi(SYS_GETPROCS, i, (long)&pi, 0) == 1; i++) {
        const char *w = "THREADTEST";
        int k = 0;
        while (w[k] && pi.name[k] == w[k]) k++;
        if (w[k] || (long)pi.pid == me) continue;
        (*left)++;
        if (pi.state == 4) (*zomb)++;            /* THREAD_ZOMBIE */
    }
}

/* Wait (bounded) until *w == 0, i.e. the thread's clear_tid was written. */
static int join(volatile unsigned *w, uint64_t bound_ms) {
    uint64_t end = now() + bound_ms * MS;
    while (*w != 0) {
        if (now() > end) return 0;
        nsi(SYS_YIELD, 0, 0, 0);
    }
    return 1;
}

__attribute__((noreturn)) static void texit(void) {
    nsi(SYS_THREAD_EXIT, 0, 0, 0);
    for (;;) { }
}

struct fx_ts { long sec, nsec; };

static long fx(volatile uint32_t *w, long op, long val, const struct fx_ts *ts) {
    return nsi6(SYS_FUTEX, (long)w, op, val, (long)ts, 0, 0);
}

/* ---- thread bodies (entered with RSP = stack top, RDI = arg) ---- */

__attribute__((noreturn, force_align_arg_pointer)) static void t_store(long arg) {
    struct shared *s = (struct shared *)arg;
    *(volatile long *)s->a_page = 0xC0FFEE;   /* private page: seen only if
                                              * the address space is shared */
    texit();
}

__attribute__((noreturn, force_align_arg_pointer)) static void t_tls(long arg) {
    struct shared *s = (struct shared *)arg;
    uint64_t fs0;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(fs0));
    /* Which block is ours is told by the self-pointer itself. */
    if (fs0 == (uint64_t)&s->tls[0][0] || fs0 == (uint64_t)&s->tls[1][0])
        __atomic_add_fetch(&s->tls_ok, 1, __ATOMIC_SEQ_CST);
    texit();
}

__attribute__((noreturn, force_align_arg_pointer)) static void t_nop(long arg) {
    (void)arg;
    texit();
}

/* The calling CPU's initial APIC id (CPUID.1:EBX[31:24]); ring 3 may read it.
 * Diagnostic only (DDR-1181 §4.3): P1 has no way to choose a CPU. */
static long cpu_id(void) {
    uint32_t a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return (long)(b >> 24);
}

/* DDR-1186: run the calling thread on `cpu` only. */
static void pin(long cpu) {
    if (nsi(SYS_SCHED_CPU, 1, cpu, 0) != 0) fail("pin");
}

__attribute__((noreturn, force_align_arg_pointer)) static void t_writer(long arg) {
    struct shared *s = (struct shared *)arg;
    volatile uint64_t *p = (volatile uint64_t *)s->page;
    pin(CPU_LOOP);
    s->lcpu = cpu_id();
    for (;;) {
        long f = s->done;
        p[0] = PAT;                      /* faults once the RO entry is seen */
        s->count++;
        if (f) s->late++;
    }
}

__attribute__((noreturn, force_align_arg_pointer)) static void t_reader(long arg) {
    struct shared *s = (struct shared *)arg;
    volatile uint64_t *p = (volatile uint64_t *)s->page;
    pin(CPU_LOOP);
    s->lcpu = cpu_id();
    for (;;) {
        long f = s->done;
        uint64_t v = p[0];               /* faults once the entry is gone */
        s->count++;
        if (f) s->late++;
        if (v != PAT) s->poison++;
    }
}

__attribute__((noreturn, force_align_arg_pointer)) static void t_crash(long arg) {
    struct shared *s = (struct shared *)arg;
    while (!s->go) nsi(SYS_YIELD, 0, 0, 0);
    *(volatile int *)0 = 1;              /* fatal fault in this thread */
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer)) static void t_copier(long arg) {
    struct shared *s = (struct shared *)arg;
    /* DDR-1181 §4.1: the destination starts 256 bytes into page A and ends
     * in page B, which the sibling toggles. A pre-check -EFAULT writes
     * nothing; a fault on B after the pre-check passed (the fixup path)
     * leaves A's tail written. A's tail is zeroed before every copy. */
    volatile unsigned char *a = (volatile unsigned char *)s->page;
    pin(CPU_OP);                         /* DDR-1186: the toggler is on CPU_LOOP */
    uint64_t end = now() + 1500 * MS;
    while (now() < end) {
        for (int i = 256; i < 4096; i++) a[i] = 0;
        if (nsi(SYS_DMESG, (long)s->page + 256, 4096, 0) == -EFAULT) {
            s->efault++;
            for (int i = 256; i < 4096; i++)
                if (a[i]) { s->midcopy++; break; }
        }
    }
    s->gdone = 1;
    texit();
}

__attribute__((noreturn, force_align_arg_pointer)) static void t_fwait(long arg) {
    struct shared *s = (struct shared *)arg;
    s->frc = fx(&s->fw, FUTEX_WAIT | FUTEX_PRIVATE, 0, 0);
    texit();
}

__attribute__((noreturn, force_align_arg_pointer)) static void t_fwait_d(long arg) {
    struct shared *s = (struct shared *)arg;
    __atomic_add_fetch(&s->fq, 1, __ATOMIC_SEQ_CST);
    fx(&s->fdw, FUTEX_WAIT | FUTEX_PRIVATE, 0, 0);
    texit();
}

__attribute__((noreturn, force_align_arg_pointer)) static void t_slow(long arg) {
    (void)arg;
    uint64_t e = now() + 30 * MS;        /* the creator is asleep by now */
    while (now() < e) nsi(SYS_YIELD, 0, 0, 0);
    texit();
}

__attribute__((noreturn, force_align_arg_pointer)) static void on_usr1(long sig) {
    (void)sig;
    nsi(SYS_SIGRETURN, 0, 0, 0);
    for (;;) { }
}

/* Bounded spin until *f is set; 0 on expiry. */
static int await(volatile long *f, uint64_t bound_ms) {
    uint64_t end = now() + bound_ms * MS;
    while (!*f) {
        if (now() > end) return 0;
        nsi(SYS_YIELD, 0, 0, 0);
    }
    return 1;
}

static void settle(uint64_t ms) {
    uint64_t e = now() + ms * MS;
    while (now() < e) nsi(SYS_YIELD, 0, 0, 0);
}

/* ---- parent side: run one child and judge how it ended ---- */

/* 1 if the child died by itself within `bound_ms`, 0 if it had to be killed. */
static int reap(long pid, uint64_t bound_ms) {
    int st = 0;
    uint64_t end = now() + bound_ms * MS;
    for (;;) {
        long r = nsi(SYS_WAIT4, pid, (long)&st, WNOHANG);
        if (r == pid) return 1;
        if (r != -11 /* -EAGAIN */) fail("wait4");
        if (now() > end) break;
        nsi(SYS_YIELD, 0, 0, 0);
    }
    nsi(SYS_KILL, pid, SIGKILL, 0);
    end = now() + 5000 * MS;             /* a SIGKILLed child must still be reaped */
    while (nsi(SYS_WAIT4, pid, (long)&st, WNOHANG) != pid) {
        if (now() > end) fail("killed child never reaped");
        nsi(SYS_YIELD, 0, 0, 0);
    }
    return 0;
}

/* Child body for D (write=1) and E/M (write=0). Never returns. This thread
 * runs on CPU_OP and the loop thread on CPU_LOOP (DDR-1186). */
__attribute__((noreturn)) static void child_tlb(struct shared *s, int write) {
    pin(CPU_OP);
    s->page = (uint64_t)map(4096, MAP_PRIVATE);
    *(volatile uint64_t *)s->page = PAT;
    spawn(write ? t_writer : t_reader, (long)s, 0, 0);
    uint64_t end = now() + 1000 * MS;
    while (s->count < 2000) {            /* the loop thread is running and   */
        if (now() > end) fail("loop thread never ran");   /* its TLB is warm */
        nsi(SYS_YIELD, 0, 0, 0);
    }
    /* DDR-1186 §4: the two CPUs still time-share with the rest of the boot,
     * so prove the loop thread runs in parallel right now. No syscall in the
     * window: the count can only advance if it runs on the other CPU, and a
     * preemption of this thread spans a vDSO tick. Capped at 600 ms. */
    uint64_t pend = now() + 600 * MS;
    while (!s->placed && now() < pend) {
        uint64_t t0 = now();
        long c0 = s->count;
        while (now() == t0 && s->count - c0 < 1000)
            __asm__ volatile("" ::: "memory");
        if (s->count - c0 >= 1000 && now() == t0)
            s->placed = 1;
        else
            nsi(SYS_YIELD, 0, 0, 0);
    }
    s->ccpu = cpu_id();
    long rc = write ? nsi(SYS_MPROTECT, (long)s->page, 4096, PROT_READ)
                    : nsi(SYS_MUNMAP, (long)s->page, 4096, 0);
    if (rc != 0) fail(write ? "mprotect" : "munmap");
    s->done = 1;
    for (;;) __asm__ volatile("pause");  /* ended only by the group kill */
}

static void clear(struct shared *s) {
    s->go = s->done = s->count = s->late = s->poison = s->efault = s->gdone = 0;
    s->midcopy = s->placed = 0;
    s->lcpu = -1;
    s->ccpu = -2;
}

/* Arm C workload for pm_measure (DDR-1185): 64 create/join/munmap cycles,
 * then wait (up to 2 s) until the reaper has freed every thread, so the
 * meter's fingerprint and settle see none of ours. */
struct c_ctx {
    struct shared *s;
    long unmap_fail, unmap_rc, left0, zomb0, reap_ms;
};
static long c_cycles(void *ctx) {
    struct c_ctx *c = (struct c_ctx *)ctx;
    for (int i = 0; i < 64; i++) {
        c->s->c_ctid = 1;
        long stk = spawn(t_nop, 0, 0, (uint64_t)&c->s->c_ctid);
        if (!join(&c->s->c_ctid, 2000)) fail("C join");
        long ur = nsi(SYS_MUNMAP, stk, STK, 0);
        if (ur != 0) {                    /* a failed unmap leaks the stack */
            if (!c->unmap_fail) c->unmap_rc = ur;
            c->unmap_fail++;
        }
    }
    long left, zomb;
    thr_left(&c->left0, &c->zomb0);
    uint64_t r0 = now();
    for (;;) {
        thr_left(&left, &zomb);
        if (left == 0 || now() - r0 > 2000 * MS) break;
        nsi(SYS_YIELD, 0, 0, 0);
    }
    c->reap_ms = (long)((now() - r0) / MS);
    return 64;                          /* one created thread tcb per cycle */
}

/* ---- DDR-1193 (C1.4 P3a): psync arms SA, SW, SB, SC, SH, SD ----
 *
 * Each arm runs in a forked child (DDR-1193 Amendment 1 item 1). The child's
 * main thread orchestrates; its threads pin with SYS_SCHED_CPU and confirm
 * the CPU (item 2). Results go to a MAP_SHARED page the parent reads after
 * reap(). A mutant that leaves threads asleep forever ends in the child's
 * own 5 s bound, or in reap()'s SIGKILL, and prints timeout=1. */
struct sy {
    pmutex   m;
    psem     sem, sem2;
    pcond    cv;
    pbarrier bar;
    volatile long idx;          /* next thread index                       */
    volatile long placed;       /* threads that confirmed their CPU        */
    volatile long go, stop, done, to;
    volatile long ctr;          /* SA: the NON-atomic counter              */
    volatile long rdy[4];       /* "about to block" flags                  */
    volatile long woke;         /* SW: lock returned; SC: real wakes (rc 0) */
    volatile long got;          /* SB part 1                               */
    volatile long inside, maxin, lingers;   /* SB part 2                   */
    volatile long turn, rounds, xn;         /* SH                          */
    volatile long phase[4], crossed[4], bad; /* SD                         */
    volatile long asleep;
};

static void sy_clear(struct sy *y) {
    volatile uint64_t *w = (volatile uint64_t *)y;
    for (unsigned long k = 0; k < sizeof *y / 8; k++) w[k] = 0;
}

/* Pin to `cpu` and count the placement only if GET confirms it (Amendment 1
 * item 2: which CPU each thread ran on, not that they ran at once). */
static void sy_place(struct sy *y, long cpu) {
    pin(cpu);
    if (nsi(SYS_SCHED_CPU, 0, 0, 0) == cpu)
        __atomic_add_fetch(&y->placed, 1, __ATOMIC_SEQ_CST);
}

static long sy_idx(struct sy *y) { return __atomic_fetch_add(&y->idx, 1, __ATOMIC_SEQ_CST); }

__attribute__((noreturn)) static void sy_exit(struct sy *y) {
    __atomic_add_fetch(&y->done, 1, __ATOMIC_SEQ_CST);
    texit();
}

static long spawn_t(void (*fn)(long), long arg) {
    long stk = map(STK, MAP_PRIVATE);
    long tid = nsi6(SYS_THREAD_CREATE, (long)fn, stk + STK, arg, 0, 0, 0);
    if (tid < 0) fail("create");
    return tid;
}

/* Bounded spin until *v >= n; 0 on expiry. */
static int sy_until(volatile long *v, long n, uint64_t bound_ms) {
    uint64_t end = now() + bound_ms * MS;
    while (__atomic_load_n(v, __ATOMIC_SEQ_CST) < n) {
        if (now() > end) return 0;
        nsi(SYS_YIELD, 0, 0, 0);
    }
    return 1;
}

static int sy_until32(volatile uint32_t *v, uint32_t n, uint64_t bound_ms) {
    uint64_t end = now() + bound_ms * MS;
    while (__atomic_load_n(v, __ATOMIC_SEQ_CST) < n) {
        if (now() > end) return 0;
        nsi(SYS_YIELD, 0, 0, 0);
    }
    return 1;
}

/* 1 once SYS_GETPROCS shows thread `tid` THREAD_BLOCKED (3). The tid that
 * SYS_THREAD_CREATE returns is the pid GETPROCS lists (Amendment 1 item 3). */
static int sy_blocked(long tid, uint64_t bound_ms) {
    struct { uint32_t pid, ppid, state, flags; char name[16];
             uint64_t run_ticks, dispatches; } pi;
    uint64_t end = now() + bound_ms * MS;
    for (;;) {
        for (long i = 0; i < 4096 && nsi(SYS_GETPROCS, i, (long)&pi, 0) == 1; i++)
            if ((long)pi.pid == tid) {
                if (pi.state == 3) return 1;
                break;
            }
        if (now() > end) return 0;
        nsi(SYS_YIELD, 0, 0, 0);
    }
}

/* SA: 4 threads, one per CPU, 20 000 non-atomic increments each under one pmutex. */
__attribute__((noreturn, force_align_arg_pointer)) static void t_sa(long arg) {
    struct sy *y = (struct sy *)arg;
    sy_place(y, sy_idx(y) & 3);
    while (!y->go) nsi(SYS_YIELD, 0, 0, 0);
    for (int k = 0; k < 20000; k++) {
        pmutex_lock(&y->m);
        y->ctr = y->ctr + 1;              /* load, add, store: not atomic */
        pmutex_unlock(&y->m);
    }
    sy_exit(y);
}

static void b_sa(struct sy *y) {
    pmutex_init(&y->m);
    for (int i = 0; i < 4; i++) spawn_t(t_sa, (long)y);
    sy_until(&y->idx, 4, 2000);
    y->go = 1;
    if (!sy_until(&y->done, 4, 10000)) y->to = 1;
}

/* SW: the waiter sleeps in pmutex_lock; ONE unlock must wake it. */
__attribute__((noreturn, force_align_arg_pointer)) static void t_sw(long arg) {
    struct sy *y = (struct sy *)arg;
    sy_idx(y);
    sy_place(y, 1);
    y->rdy[0] = 1;
    pmutex_lock(&y->m);                   /* held by the child's main thread */
    y->woke = 1;
    pmutex_unlock(&y->m);
    sy_exit(y);
}

static void b_sw(struct sy *y) {
    pmutex_init(&y->m);
    sy_place(y, 0);
    pmutex_lock(&y->m);
    long tid = spawn_t(t_sw, (long)y);
    /* asleep: the waiter reached the futex_wait in the slow path (waits >= 1)
     * and GETPROCS then shows it BLOCKED. */
    y->asleep = sy_until(&y->rdy[0], 1, 2000) &&
                sy_until32(&y->m.waits, 1, 2000) && sy_blocked(tid, 2000);
    pmutex_unlock(&y->m);                 /* the one and only unlock */
    if (!sy_until(&y->woke, 1, 1000)) y->to = 1;
}

/* SB part 1: a consumer that is asleep before the first post, then 4 000
 * posts and 4 000 waits. Part 2: a psem of 2 guards a section 4 threads
 * enter 500 times each; a holder that is alone lingers up to one vDSO tick
 * for others, on its first 20 entries (Amendment 1 item 4). */
__attribute__((noreturn, force_align_arg_pointer)) static void t_sb1(long arg) {
    struct sy *y = (struct sy *)arg;
    sy_idx(y);
    sy_place(y, 1);
    y->rdy[0] = 1;
    for (int k = 0; k < 4000; k++) {
        psem_wait(&y->sem);
        __atomic_add_fetch(&y->got, 1, __ATOMIC_SEQ_CST);
    }
    sy_exit(y);
}

__attribute__((noreturn, force_align_arg_pointer)) static void t_sb2(long arg) {
    struct sy *y = (struct sy *)arg;
    sy_place(y, sy_idx(y) & 3);
    while (!y->go) nsi(SYS_YIELD, 0, 0, 0);
    for (int k = 0; k < 500; k++) {
        psem_wait(&y->sem2);
        long in = __atomic_add_fetch(&y->inside, 1, __ATOMIC_SEQ_CST);
        long mx = __atomic_load_n(&y->maxin, __ATOMIC_SEQ_CST);
        while (in > mx && !__atomic_compare_exchange_n(&y->maxin, &mx, in, 0,
                                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            ;
        if (k < 20) {
            uint64_t t0 = now();
            while (__atomic_load_n(&y->inside, __ATOMIC_SEQ_CST) < 3 && now() == t0)
                __asm__ volatile("pause");
            __atomic_add_fetch(&y->lingers, 1, __ATOMIC_RELAXED);
        }
        __atomic_sub_fetch(&y->inside, 1, __ATOMIC_SEQ_CST);
        psem_post(&y->sem2);
    }
    sy_exit(y);
}

static void b_sb(struct sy *y) {
    psem_init(&y->sem, 0);
    sy_place(y, 0);
    long tid = spawn_t(t_sb1, (long)y);
    y->asleep = sy_until(&y->rdy[0], 1, 2000) &&
                sy_until32(&y->sem.nwait, 1, 2000) && sy_blocked(tid, 2000);
    for (int k = 0; k < 4000; k++) psem_post(&y->sem);
    if (!sy_until(&y->done, 1, 5000)) { y->to = 1; return; }
    psem_init(&y->sem2, 2);
    y->idx = 0;
    for (int i = 0; i < 4; i++) spawn_t(t_sb2, (long)y);
    sy_until(&y->idx, 4, 2000);
    y->go = 1;
    if (!sy_until(&y->done, 5, 5000)) y->to = 1;
}

/* SC: 3 waiters asleep on one pcond; ONE broadcast must wake all three. */
__attribute__((noreturn, force_align_arg_pointer)) static void t_sc(long arg) {
    struct sy *y = (struct sy *)arg;
    long i = sy_idx(y);
    sy_place(y, i + 1);
    pmutex_lock(&y->m);
    y->rdy[i] = 1;                        /* next blocking call: the cond wait */
    if (pcond_wait(&y->cv, &y->m) == 0)   /* a real wake, not -EAGAIN */
        __atomic_add_fetch(&y->woke, 1, __ATOMIC_SEQ_CST);
    pmutex_unlock(&y->m);
    sy_exit(y);
}

static void b_sc(struct sy *y) {
    long tid[3];
    pmutex_init(&y->m);
    pcond_init(&y->cv);
    for (int i = 0; i < 3; i++) tid[i] = spawn_t(t_sc, (long)y);
    for (int i = 0; i < 3; i++)
        if (sy_until(&y->rdy[i], 1, 2000) && sy_blocked(tid[i], 2000)) y->asleep++;
    pcond_broadcast(&y->cv);              /* the one and only broadcast */
    if (!sy_until(&y->done, 3, 5000)) y->to = 1;
}

/* SH: two threads hand a turn back and forth 1 000 times under pmutex +
 * pcond_signal; a third contends for the pmutex the whole time, so a waiter
 * is often asleep on it when pcond_wait relocks (Amendment 1 item 5). */
__attribute__((noreturn, force_align_arg_pointer)) static void t_sh(long arg) {
    struct sy *y = (struct sy *)arg;
    long me = sy_idx(y);
    sy_place(y, me + 1);
    while (!y->go) nsi(SYS_YIELD, 0, 0, 0);
    if (me == 2) {                        /* the contender */
        while (!y->stop) {
            pmutex_lock(&y->m);
            y->xn++;
            pmutex_unlock(&y->m);
            nsi(SYS_YIELD, 0, 0, 0);      /* a window for a woken waiter: no starvation */
        }
        sy_exit(y);
    }
    for (int r = 0; r < 1000; r++) {
        pmutex_lock(&y->m);
        while (y->turn != me)
            pcond_wait(&y->cv, &y->m);
        y->turn = 1 - me;
        if (me == 0) y->rounds++;
        pcond_signal(&y->cv);
        pmutex_unlock(&y->m);
    }
    sy_exit(y);
}

static void b_sh(struct sy *y) {
    pmutex_init(&y->m);
    pcond_init(&y->cv);
    for (int i = 0; i < 3; i++) spawn_t(t_sh, (long)y);
    sy_until(&y->idx, 3, 2000);
    y->go = 1;
    if (!sy_until(&y->done, 2, 5000)) { y->to = 1; return; }
    y->stop = 1;
    if (!sy_until(&y->done, 3, 1000)) y->to = 1;
}

/* SD: 4 threads cross one pbarrier 100 times; after each crossing every
 * thread checks that all four phase counters are >= its own. */
__attribute__((noreturn, force_align_arg_pointer)) static void t_sd(long arg) {
    struct sy *y = (struct sy *)arg;
    long i = sy_idx(y);
    sy_place(y, i & 3);
    while (!y->go) nsi(SYS_YIELD, 0, 0, 0);
    for (long p = 1; p <= 100; p++) {
        y->phase[i] = p;
        pbarrier_wait(&y->bar);
        for (int j = 0; j < 4; j++)
            if (y->phase[j] < p) __atomic_add_fetch(&y->bad, 1, __ATOMIC_SEQ_CST);
        y->crossed[i] = p;
    }
    sy_exit(y);
}

static void b_sd(struct sy *y) {
    pbarrier_init(&y->bar, 4);
    for (int i = 0; i < 4; i++) spawn_t(t_sd, (long)y);
    sy_until(&y->idx, 4, 2000);
    y->go = 1;
    if (!sy_until(&y->done, 4, 5000)) y->to = 1;
}

/* Run one arm body in a forked child. Returns timeout (child's own bound,
 * or reap() had to kill it) and the elapsed ms. */
static long sy_run(struct sy *y, void (*body)(struct sy *), uint64_t bound_ms, long *ms) {
    sy_clear(y);
    uint64_t t0 = now();
    long p = nsi(SYS_FORK, 0, 0, 0);
    if (p < 0) fail("fork");
    if (p == 0) {
        body(y);
        nsi(SYS_EXIT, 0, 0, 0);           /* group exit: ends any stuck thread */
        for (;;) { }
    }
    long ended = reap(p, bound_ms);
    *ms = (long)((now() - t0) / MS);
    return y->to || !ended;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    struct shared *s = (struct shared *)map(4096, MAP_SHARED);

    /* A */
    /* A: the store lands in a MAP_PRIVATE page of the creator, so a thread
     * on a COW copy of the address space (T1) is not seen (DDR-1181 §4.1). */
    s->a_page = (uint64_t)map(4096, MAP_PRIVATE);
    *(volatile long *)s->a_page = 0;
    s->a_ctid = 1;
    spawn(t_store, (long)s, 0, (uint64_t)&s->a_ctid);
    long joined = join(&s->a_ctid, 2000);
    say("PRADYOS_THREAD_A shared=%d joined=%d\n",
        (long)(*(volatile long *)s->a_page == 0xC0FFEE), (long)(joined));

    /* B */
    s->tls_ok = 0;
    s->tls[0][0] = (uint64_t)&s->tls[0][0];
    s->tls[1][0] = (uint64_t)&s->tls[1][0];
    /* The creator gets a TLS block of its own, so a thread that inherited the
     * creator's FS base instead of its `tls` reads a self-pointer that is
     * neither of the two (tls_ok < 2) rather than faulting on %fs:0 = 0. */
    s->tls[2][0] = (uint64_t)&s->tls[2][0];
    if (nsi(SYS_SET_TLS, (long)&s->tls[2][0], 0, 0) != 0) fail("set_tls");
    s->a_ctid = 1;
    s->c_ctid = 1;
    spawn(t_tls, (long)s, (uint64_t)&s->tls[0][0], (uint64_t)&s->a_ctid);
    spawn(t_tls, (long)s, (uint64_t)&s->tls[1][0], (uint64_t)&s->c_ctid);
    join(&s->a_ctid, 2000);
    join(&s->c_ctid, 2000);
    say("PRADYOS_THREAD_B tls_ok=%d\n", (long)(s->tls_ok));

    /* C: 8 warm-up cycles, then 64 create/join/munmap cycles measured by
     * pm_measure (DDR-1185). The PMM free count is system-wide and the rest
     * of the boot runs meanwhile: run 40217768311 lost 293 pages in a 2 s
     * idle window, and an idle control taken at another time (the earlier
     * C2) read that traffic as net=470 on one head and as green on a
     * semantically identical one. Only a trial with quiet traffic around it
     * counts, with no correction. A leaked tcb + kernel stack is ~6 pages,
     * 64 of them ~380; the bound of 32 is far below. */
    struct c_ctx c = { s, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 8; i++) {
        s->c_ctid = 1;
        long stk = spawn(t_nop, 0, 0, (uint64_t)&s->c_ctid);
        if (!join(&s->c_ctid, 2000)) fail("C join");
        nsi(SYS_MUNMAP, stk, STK, 0);
    }
    struct pm_result pr;
    int conclusive = pm_measure(c_cycles, &c, &pr);
    long left = 0, zomb = 0;
    thr_left(&left, &zomb);
    say("[thread] C drop=%d trials=%d rej_pre=%d rej_post=%d rej_procs=%d conclusive=%d\n",
        pr.drop, pr.trials, pr.rej_pre, pr.rej_post, pr.rej_procs, (long)conclusive);
    say("[thread] C after_cycles left=%d zombie=%d reap_ms=%d\n", c.left0, c.zomb0, c.reap_ms);
    say("[thread] C munmap_fail=%d first_rc=%d\n", c.unmap_fail, c.unmap_rc);
    say("PRADYOS_THREAD_C leftover=%d zombie=%d\n", left, zomb);
    line1("PRADYOS_THREAD_C2 net_leak_ok=", conclusive && pr.drop <= 32);

    /* P (DDR-1186): pin to every present CPU and read the CPU back; bad
     * arguments are refused. */
    {
        struct pm_sysinfo si;
        pm_sys(71 /* SYS_SYSINFO */, (long)&si, 0);
        long ncpu = (long)si.cpu_count, ok = ncpu > CPU_LOOP, seen = 0;
        for (long c = 0; c < ncpu; c++) {
            if (nsi(SYS_SCHED_CPU, 1, c, 0) != 0) ok = 0;
            for (int r = 0; r < 3; r++) {
                nsi(SYS_YIELD, 0, 0, 0);
                if (nsi(SYS_SCHED_CPU, 0, 0, 0) == c) seen++;
                else ok = 0;
            }
        }
        long einval = nsi(SYS_SCHED_CPU, 1, ncpu, 0) == -EINVAL &&
                      nsi(SYS_SCHED_CPU, 1, -1, 0) == -EINVAL &&
                      nsi(SYS_SCHED_CPU, 9, 0, 0) == -EINVAL;
        if (nsi(SYS_SCHED_CPU, 2, 0, 0) != 0) ok = 0;
        say("[thread] P cpus=%d seen=%d of %d\n", ncpu, seen, ncpu * 3);
        say("PRADYOS_THREAD_P pinned_ok=%d einval=%d\n", ok, einval);
    }

    /* D, then E/M: TRIALS forked children each. */
    for (int arm = 0; arm < 2; arm++) {
        long late = 0, poison = 0, faulted = 1, reached = 0, same = 0, placed = 0;
        for (int k = 0; k < TRIALS; k++) {
            clear(s);
            long pid = nsi(SYS_FORK, 0, 0, 0);
            if (pid < 0) fail("fork");
            if (pid == 0) child_tlb(s, arm == 0);
            /* DDR-1181 §4.2: the fault bound starts at the operation, not at
             * fork; the warm-up is bounded on its own. */
            uint64_t op_end = now() + 3000 * MS;
            while (!s->done && now() < op_end) nsi(SYS_YIELD, 0, 0, 0);
            if (s->done) reached++;
            if (!reap(pid, 1000)) faulted = 0;
            /* The loop thread's CPU id (CPUID) against the operating
             * thread's, at the operation: a pin that did not hold. */
            if (s->lcpu == s->ccpu) same++;
            /* Only concurrency-proven trials, and only on two CPUs: the
             * vDSO witness alone once counted a same-CPU trial (§4.4). */
            if (!s->placed || s->lcpu == s->ccpu) continue;
            placed++;
            late += s->late;
            poison += s->poison;
        }
        long pok = placed >= PLACED_MIN;
        if (arm == 0) {
            say("[thread] D reached=%d placed=%d of %d cpu_same=%d\n",
                reached, placed, (long)TRIALS, same);
            say("PRADYOS_THREAD_D prot_stale=%d faulted=%d placed_ok=%d\n", late, faulted, pok);
        } else {
            say("[thread] E reached=%d placed=%d of %d cpu_same=%d\n",
                reached, placed, (long)TRIALS, same);
            say("PRADYOS_THREAD_E stale=%d faulted=%d placed_ok=%d\n", late, faulted, pok);
            say("PRADYOS_THREAD_M poison=%d\n", poison);
        }
    }

    /* F: the leader spins in ring 3 while a sibling faults. */
    clear(s);
    long fp = nsi(SYS_FORK, 0, 0, 0);
    if (fp < 0) fail("fork");
    if (fp == 0) {
        spawn(t_crash, (long)s, 0, 0);
        s->go = 1;
        for (;;) __asm__ volatile("pause");
    }
    long gk = reap(fp, 2000);            /* the child's trap line prints here */
    line1("PRADYOS_THREAD_F group_killed=", gk);

    /* G: copyout into a page a sibling keeps unmapping and remapping. The
     * copier runs on CPU_OP and the toggler on CPU_LOOP (DDR-1186): on one
     * CPU the toggler never runs inside the copier's syscall (DDR-1181 §4.3). */
    clear(s);
    long gp = nsi(SYS_FORK, 0, 0, 0);
    if (gp < 0) fail("fork");
    if (gp == 0) {
        /* Pages A and B are two regions: munmap takes a whole region by its
         * base (sys_mmap.c vma_find), so B alone can only come and go if it is
         * one (DDR-1181 §4.2). The bump allocator places B right after A. */
        long pa = map(4096, MAP_PRIVATE);
        uint64_t pb = (uint64_t)map(4096, MAP_PRIVATE);
        if (pb != (uint64_t)pa + 4096) fail("G layout");
        s->page = (uint64_t)pa;
        pin(CPU_LOOP);
        spawn(t_copier, (long)s, 0, 0);
        while (!s->gdone) {                  /* only B comes and goes */
            nsi(SYS_MUNMAP, (long)pb, 4096, 0);
            if (nsi6(SYS_MMAP, (long)pb, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != (long)pb)
                fail("G remap");
        }
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }
    long survived = reap(gp, 5000);
    say("[thread] G efault=%d midcopy=%d\n", (long)(s->efault), (long)(s->midcopy));
    say("PRADYOS_THREAD_G survived=%d midcopy_ok=%d\n", (long)(survived),
        (long)(s->midcopy >= 1));

    /* ---- DDR-1181 Amendment 1 (P2): futex ---- */
    /* FA: a thread sleeps in WAIT; WAKE(1) returns 1 once it is queued. */
    s->fw = 0; s->frc = 99; s->a_ctid = 1;
    spawn(t_fwait, (long)s, 0, (uint64_t)&s->a_ctid);
    long woke = 0;
    {
        uint64_t end = now() + 2000 * MS;
        while (now() < end && !woke) {
            if (fx(&s->fw, FUTEX_WAKE | FUTEX_PRIVATE, 1, 0) == 1) woke = 1;
            else nsi(SYS_YIELD, 0, 0, 0);
        }
    }
    join(&s->a_ctid, 2000);
    say("PRADYOS_FUTEX_A woke=%d rc=%d\n", (long)(woke), (long)(s->frc));

    /* FB: a value mismatch returns at once (a child: a mutant would sleep). */
    s->fbw = 5; s->brc = 1;
    long bp = nsi(SYS_FORK, 0, 0, 0);
    if (bp < 0) fail("fork");
    if (bp == 0) {
        s->brc = fx(&s->fbw, FUTEX_WAIT, 4, 0);
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }
    reap(bp, 2000);
    say("PRADYOS_FUTEX_B eagain=%d\n", (long)(s->brc));

    /* FC: a 55 ms timeout returns -ETIMEDOUT and is never early (DDR-1195
     * §2). The sleep is measured with the TSC, not the 10 ms vDSO clock, and
     * the WAIT starts ~0.5 ms before a tick edge: the worst case for a
     * deadline counted in tick edges. */
    s->fcw = 0; s->crc = 1; s->cms = 0; s->ctsc = 0;
    long cp = nsi(SYS_FORK, 0, 0, 0);
    if (cp < 0) fail("fork");
    if (cp == 0) {
        uint64_t per[3];
        for (int w = 0; w < 3; w++) {          /* median of three 100 ms windows */
            uint64_t c0 = tsc_at_edge(), v0 = now();
            while (now() < v0 + 100 * MS) __asm__ volatile("pause");
            uint64_t c1 = tsc();
            per[w] = (c1 - c0) / 100;
        }
        uint64_t lo = per[0] < per[1] ? per[0] : per[1];
        uint64_t hi = per[0] < per[1] ? per[1] : per[0];
        uint64_t per_ms = per[2] < lo ? lo : (per[2] > hi ? hi : per[2]);
        s->ctsc = (long)per_ms;
        uint64_t c2 = tsc_at_edge();
        while (tsc() - c2 < per_ms * 95 / 10) __asm__ volatile("pause");
        struct fx_ts ts = { 0, 55 * (long)MS };
        uint64_t c3 = tsc();
        s->crc = fx(&s->fcw, FUTEX_WAIT, 0, &ts);
        uint64_t c4 = tsc();
        s->cms = per_ms ? (long)((c4 - c3) * 100 / per_ms) : 0;   /* 1/100 ms */
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }
    reap(cp, 5000);
    say("[thread] C ms_x100=%d tsc_per_ms=%d\n", (long)(s->cms), (long)(s->ctsc));
    say("PRADYOS_FUTEX_C timedout=%d slept_ok=%d\n", (long)(s->crc),
        (long)(s->ctsc > 0 && s->cms >= 5500));

    /* FD: WAKE(2) never wakes more than 2 of 4 waiters. */
    s->fdw = 0; s->fq = 0;
    for (int i = 0; i < 4; i++) spawn(t_fwait_d, (long)s, 0, 0);
    {
        uint64_t end = now() + 2000 * MS;
        while (s->fq < 4 && now() < end) nsi(SYS_YIELD, 0, 0, 0);
    }
    settle(100);                         /* all four are queued */
    long over = 0, total = 0;
    {
        uint64_t end = now() + 2000 * MS;
        while (total < 4 && now() < end) {
            long r = fx(&s->fdw, FUTEX_WAKE | FUTEX_PRIVATE, 2, 0);
            if (r > 2) over = 1;
            if (r > 0) total += r;
            else nsi(SYS_YIELD, 0, 0, 0);
        }
    }
    say("PRADYOS_FUTEX_D max2=%d total=%d\n", (long)(!over), (long)(total));

    /* FE: a fork child waits on the MAP_SHARED page; the parent wakes it. */
    s->few = 0; s->erc = 1; s->ready = 0;
    long ep = nsi(SYS_FORK, 0, 0, 0);
    if (ep < 0) fail("fork");
    if (ep == 0) {
        s->ready = 1;
        s->erc = fx(&s->few, FUTEX_WAIT, 0, 0);
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }
    await(&s->ready, 2000);
    long ewoke = 0;
    {
        uint64_t end = now() + 2000 * MS;
        while (now() < end && !ewoke) {
            if (fx(&s->few, FUTEX_WAKE, 1, 0) == 1) ewoke = 1;
            else nsi(SYS_YIELD, 0, 0, 0);
        }
    }
    reap(ep, 2000);
    say("PRADYOS_FUTEX_E shared_woke=%d rc=%d\n", (long)(ewoke), (long)(s->erc));

    /* FF: a PRIVATE waiter in another process is reached by no wake of ours.
     * FG: and SIGKILL still ends it. DDR-1195 §3: the waiter must be seen
     * BLOCKED, with its WAIT not returned (frc_f == 99), before and after the
     * wakes and at the kill; otherwise both arms could pass on a child that
     * had already left. */
    s->ffw = 0; s->ready = 0; s->frc_f = 99;
    long fpp = nsi(SYS_FORK, 0, 0, 0);
    if (fpp < 0) fail("fork");
    if (fpp == 0) {
        s->ready = 1;
        s->frc_f = fx(&s->ffw, FUTEX_WAIT | FUTEX_PRIVATE, 0, 0);
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }
    long fasleep = await(&s->ready, 2000) && sy_blocked(fpp, 2000) && s->frc_f == 99;
    long leaked = fx(&s->ffw, FUTEX_WAKE | FUTEX_PRIVATE, 1, 0);
    leaked += fx(&s->ffw, FUTEX_WAKE, 1, 0);
    long fstill = fasleep && sy_blocked(fpp, 500) && s->frc_f == 99;
    say("PRADYOS_FUTEX_F private_isolated=%d asleep=%d\n",
        (long)(leaked == 0 && fstill), fstill);
    nsi(SYS_KILL, fpp, SIGKILL, 0);
    long killed = 0;
    {
        int st = 0;
        uint64_t end = now() + 2000 * MS;
        while (now() < end) {
            if (nsi(SYS_WAIT4, fpp, (long)&st, WNOHANG) == fpp) { killed = fstill; break; }
            nsi(SYS_YIELD, 0, 0, 0);
        }
    }
    say("PRADYOS_FUTEX_G killed_waiter=%d\n", (long)(killed));

    /* FH: a signal with a handler interrupts the wait. */
    s->fhw = 0; s->hrc = 1; s->hdone = 0; s->ready = 0;
    long hp = nsi(SYS_FORK, 0, 0, 0);
    if (hp < 0) fail("fork");
    if (hp == 0) {
        struct { uint64_t h, flags, mask; } act = { (uint64_t)on_usr1, 0, 0 };
        if (nsi(SYS_SIGACTION2, SIGUSR1, (long)&act, 0) != 0) nsi(SYS_EXIT, 2, 0, 0);
        s->ready = 1;
        s->hrc = fx(&s->fhw, FUTEX_WAIT, 0, 0);
        s->hdone = 1;
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }
    await(&s->ready, 2000);
    {
        uint64_t end = now() + 2000 * MS;
        while (!s->hdone && now() < end) {
            nsi(SYS_KILL, hp, SIGUSR1, 0);   /* again, if one landed before WAIT */
            settle(20);
        }
    }
    reap(hp, 2000);
    say("PRADYOS_FUTEX_H eintr=%d\n", (long)(s->hrc));

    /* FI: a sleeping joiner is woken by the CLEARTID write. */
    long iwoke = 0, itimeout = 0;
    for (int k = 0; k < 8; k++) {
        s->ictid = 1;
        long stk = spawn(t_slow, 0, 0, (uint64_t)&s->ictid);
        struct fx_ts ts = { 2, 0 };
        long r = fx(&s->ictid, FUTEX_WAIT, 1, &ts);
        if (r == 0 && s->ictid == 0) iwoke++;
        if (r == -110) itimeout++;
        join(&s->ictid, 2000);
        nsi(SYS_MUNMAP, stk, STK, 0);
    }
    say("PRADYOS_FUTEX_I cleartid_woke=%d\n", (long)(iwoke > 0 && itimeout == 0));

    /* FJ: argument checks. */
    long rej = 0;
    struct fx_ts bad = { 0, 1000000000L };
    if (fx((volatile uint32_t *)((char *)&s->fw + 1), FUTEX_WAIT, 0x7FFFFFFF, 0) == -22) rej++;   /* a mutant gets -EAGAIN, never sleeps */
    if (fx((volatile uint32_t *)0x10, FUTEX_WAIT, 0, 0) == -14) rej++;
    if (fx(&s->fw, 5, 0, 0) == -38) rej++;
    if (fx(&s->fw, FUTEX_WAIT, 0, &bad) == -22) rej++;
    say("PRADYOS_FUTEX_J rejects=%d\n", (long)(rej));

    /* ---- DDR-1193 (C1.4 P3a): psync arms ---- */
    {
        struct sy *y = (struct sy *)map(4096, MAP_SHARED);
        long ms, to;
        say("[sync] start_ms=%d\n", (long)(now() / MS));

        to = sy_run(y, b_sa, 12000, &ms);
        say("[sync] A contended=%d ms=%d timeout=%d\n", (long)(y->m.waits), ms, to);
        say("PRADYOS_SYNC_A total=%d exp=80000 contended_ok=%d placed_ok=%d\n",
            (long)(y->ctr), (long)(y->m.waits > 0), (long)(y->placed == 4));

        to = sy_run(y, b_sw, 8000, &ms);
        say("[sync] W ms=%d\n", ms);
        say("PRADYOS_SYNC_W asleep=%d woke=%d timeout=%d placed_ok=%d\n",
            (long)(y->asleep), (long)(y->woke), to, (long)(y->placed == 2));

        to = sy_run(y, b_sb, 15000, &ms);
        say("[sync] B maxin=%d lingers=%d ms=%d\n", (long)(y->maxin), (long)(y->lingers), ms);
        say("PRADYOS_SYNC_B got=%d asleep=%d maxin_ok=%d overlap=%d timeout=%d placed_ok=%d\n",
            (long)(y->got), (long)(y->asleep), (long)(y->maxin <= 2), (long)(y->maxin >= 2),
            to, (long)(y->placed == 6));

        to = sy_run(y, b_sc, 10000, &ms);
        say("[sync] C ms=%d\n", ms);
        say("PRADYOS_SYNC_C asleep=%d woke=%d timeout=%d placed_ok=%d\n",
            (long)(y->asleep), (long)(y->woke), to, (long)(y->placed == 3));

        to = sy_run(y, b_sh, 8000, &ms);
        say("[sync] H contender=%d ms=%d\n", (long)(y->xn), ms);
        say("PRADYOS_SYNC_H rounds=%d timeout=%d placed_ok=%d\n",
            (long)(y->rounds), to, (long)(y->placed == 3));

        to = sy_run(y, b_sd, 8000, &ms);
        long ph = 100;
        for (int i = 0; i < 4; i++) if (y->crossed[i] < ph) ph = y->crossed[i];
        say("[sync] D ms=%d\n", ms);
        say("PRADYOS_SYNC_D phases=%d bad=%d timeout=%d placed_ok=%d\n",
            ph, (long)(y->bad), to, (long)(y->placed == 4));
        say("[sync] end_ms=%d\n", (long)(now() / MS));
    }

    say("PRADYOS_THREAD_DONE\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
