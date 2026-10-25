/* user/threadtest.c — ring-3 thread probe for smoke-thread (DDR-1181 §4).
 *
 * FAT-rooted, opt-in (QEMU_PROBES=thread), run with 4 vCPUs. Arms:
 *   A  a store by a thread is seen by its creator; clear_tid is zeroed at exit
 *   B  two threads, two TLS blocks: each reads its own %fs:0 self-pointer
 *   C  72 create/exit cycles leak no physical pages
 *   D  DDR-1177 arm D: mprotect(PROT_READ) while a thread on another CPU writes
 *   E  DDR-1177 arm E: munmap while a thread on another CPU reads
 *   M  DDR-1177 M4: no read through a stale entry ever returns a freed frame's
 *      poison (KASAN fills freed frames with 0xDEADBEEFDEADBEEF)
 *   F  a fault in one thread ends the whole process
 *   G  DDR-1181 §3.7: a kernel copyout racing munmap of its buffer is
 *      -EFAULT, never a kernel panic
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

#define nsi pradyos_syscall

#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_ANONYMOUS  0x20
#define WNOHANG        1
#define SIGKILL        9
#define EFAULT         14

#define VDSO_USER_VA   0x00007FFFFFF00000ull
#define MS             1000000ull
#define STK            16384
#define PAT            0x5EED5EED5EED5EEDull
#define TRIALS         16

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
    volatile long a_store;     /* arm A */
    volatile unsigned a_ctid;  /* arm A: zeroed by the kernel at thread exit */
    volatile unsigned c_ctid;  /* arm C */
};

static long spawn(void (*fn)(long), long arg, uint64_t tls, uint64_t ctid) {
    long stk = map(STK, MAP_PRIVATE);
    long tid = nsi6(SYS_THREAD_CREATE, (long)fn, stk + STK, arg, (long)tls, (long)ctid, 0);
    if (tid < 0) fail("create");
    return stk;
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

/* ---- thread bodies (entered with RSP = stack top, RDI = arg) ---- */

__attribute__((noreturn, force_align_arg_pointer)) static void t_store(long arg) {
    struct shared *s = (struct shared *)arg;
    s->a_store = 0xC0FFEE;
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

__attribute__((noreturn, force_align_arg_pointer)) static void t_spin(long arg) {
    (void)arg;
    for (;;) __asm__ volatile("pause");
}

__attribute__((noreturn, force_align_arg_pointer)) static void t_writer(long arg) {
    struct shared *s = (struct shared *)arg;
    volatile uint64_t *p = (volatile uint64_t *)s->page;
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
    uint64_t end = now() + 1500 * MS;
    while (now() < end) {
        if (nsi(SYS_DMESG, (long)s->page, 4096, 0) == -EFAULT)
            s->efault++;
    }
    s->gdone = 1;
    texit();
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

/* Child body for D (write=1) and E/M (write=0). Never returns. */
__attribute__((noreturn)) static void child_tlb(struct shared *s, int write) {
    s->page = (uint64_t)map(4096, MAP_PRIVATE);
    *(volatile uint64_t *)s->page = PAT;
    spawn(t_spin, 0, 0, 0);              /* keep the other vCPUs busy, so the */
    spawn(t_spin, 0, 0, 0);              /* loop thread is not on ours        */
    spawn(write ? t_writer : t_reader, (long)s, 0, 0);
    uint64_t end = now() + 1000 * MS;
    while (s->count < 2000) {            /* the loop thread is running and   */
        if (now() > end) fail("loop thread never ran");   /* its TLB is warm */
        nsi(SYS_YIELD, 0, 0, 0);
    }
    long rc = write ? nsi(SYS_MPROTECT, (long)s->page, 4096, PROT_READ)
                    : nsi(SYS_MUNMAP, (long)s->page, 4096, 0);
    if (rc != 0) fail(write ? "mprotect" : "munmap");
    s->done = 1;
    for (;;) __asm__ volatile("pause");  /* ended only by the group kill */
}

static void clear(struct shared *s) {
    s->go = s->done = s->count = s->late = s->poison = s->efault = s->gdone = 0;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    struct shared *s = (struct shared *)map(4096, MAP_SHARED);

    /* A */
    s->a_store = 0;
    s->a_ctid = 1;
    spawn(t_store, (long)s, 0, (uint64_t)&s->a_ctid);
    long joined = join(&s->a_ctid, 2000);
    say("PRADYOS_THREAD_A shared=%d joined=%d\n", (long)(s->a_store == 0xC0FFEE), (long)(joined));

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

    /* C: 8 warm-up cycles, settle, then 64 create/join/munmap cycles. The PMM
     * free count is system-wide and the rest of the boot runs meanwhile (run
     * 39551618166: -375 pages, run 40124472347: -363, no thread left in
     * either), so the cycles are paired with an idle control window of the
     * same length and the gate bounds the DIFFERENCE. A leaked tcb + kernel
     * stack is ~6 pages, 64 of them ~380; the bound of 32 is far below. */
    struct { uint64_t total, free, used; unsigned psz, pad; } mi;
    uint64_t before = 0, t0 = 0;
    for (int i = 0; i < 72; i++) {
        if (i == 8) {
            uint64_t e = now() + 300 * MS;
            while (now() < e) nsi(SYS_YIELD, 0, 0, 0);
            if (nsi(SYS_MEMINFO, (long)&mi, 0, 0) != 0) fail("meminfo");
            before = mi.free;
            t0 = now();
        }
        s->c_ctid = 1;
        long stk = spawn(t_nop, 0, 0, (uint64_t)&s->c_ctid);
        if (!join(&s->c_ctid, 2000)) fail("C join");
        nsi(SYS_MUNMAP, stk, STK, 0);
    }
    uint64_t span = now() - t0;
    {
        uint64_t e = now() + 300 * MS;   /* the reaper frees thread zombies */
        while (now() < e) nsi(SYS_YIELD, 0, 0, 0);
    }
    if (nsi(SYS_MEMINFO, (long)&mi, 0, 0) != 0) fail("meminfo");
    long drop_cyc = (long)before - (long)mi.free;
    /* Control: the same span plus the same 300 ms settle, with no threads. */
    uint64_t cbefore = mi.free;
    {
        uint64_t e = now() + span + 300 * MS;
        while (now() < e) nsi(SYS_YIELD, 0, 0, 0);
    }
    if (nsi(SYS_MEMINFO, (long)&mi, 0, 0) != 0) fail("meminfo");
    long drop_idle = (long)cbefore - (long)mi.free;
    long net = drop_cyc - drop_idle;
    say("[thread] C drop_cycles=%d drop_idle=%d net=%d span_ms=%d\n",
        drop_cyc, drop_idle, net, (long)(span / MS));
    /* A thread whose tcb and kernel stack are never freed (T3) also stays in
     * the scheduler as a THREADTEST zombie: count them, polling up to 2 s. */
    long left = 0, zomb = 0;
    {
        struct { uint32_t pid, ppid, state, flags; char name[16];
                 uint64_t run_ticks, dispatches; } pi;
        long me = nsi(SYS_GETPID, 0, 0, 0);
        uint64_t end = now() + 2000 * MS;
        for (;;) {
            left = zomb = 0;
            for (long i = 0; i < 4096 && nsi(SYS_GETPROCS, i, (long)&pi, 0) == 1; i++) {
                const char *w = "THREADTEST";
                int k = 0;
                while (w[k] && pi.name[k] == w[k]) k++;
                if (w[k] || (long)pi.pid == me) continue;
                left++;
                if (pi.state == 4) zomb++;           /* THREAD_ZOMBIE */
            }
            if (left == 0 || now() > end) break;
            nsi(SYS_YIELD, 0, 0, 0);
        }
    }
    say("PRADYOS_THREAD_C leftover=%d zombie=%d\n", left, zomb);
    line1("PRADYOS_THREAD_C2 net_leak_ok=", net <= 32);

    /* D, then E/M: TRIALS forked children each. */
    for (int arm = 0; arm < 2; arm++) {
        long late = 0, poison = 0, faulted = 1;
        for (int k = 0; k < TRIALS; k++) {
            clear(s);
            long pid = nsi(SYS_FORK, 0, 0, 0);
            if (pid < 0) fail("fork");
            if (pid == 0) child_tlb(s, arm == 0);
            if (!reap(pid, 1000)) faulted = 0;
            late += s->late;
            poison += s->poison;
        }
        if (arm == 0) {
            say("PRADYOS_THREAD_D prot_stale=%d faulted=%d\n", (long)(late), (long)(faulted));
        } else {
            say("PRADYOS_THREAD_E stale=%d faulted=%d\n", (long)(late), (long)(faulted));
            say("PRADYOS_THREAD_M poison=%d\n", (long)(poison));
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

    /* G: copyout into a page a sibling keeps unmapping and remapping. */
    clear(s);
    long gp = nsi(SYS_FORK, 0, 0, 0);
    if (gp < 0) fail("fork");
    if (gp == 0) {
        s->page = (uint64_t)map(4096, MAP_PRIVATE);
        spawn(t_copier, (long)s, 0, 0);
        while (!s->gdone) {
            nsi(SYS_MUNMAP, (long)s->page, 4096, 0);
            if (nsi6(SYS_MMAP, (long)s->page, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != (long)s->page)
                fail("G remap");
        }
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }
    long survived = reap(gp, 5000);
    say("[thread] G efault=%d\n", (long)(s->efault));
    say("PRADYOS_THREAD_G survived=%d\n", (long)(survived));

    say("PRADYOS_THREAD_DONE\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
