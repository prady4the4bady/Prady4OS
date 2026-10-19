/* user/shmtest.c — DDR-1178 P1: anonymous MAP_SHARED across fork (C1.3).
 *
 * Four arms (DDR-1178 §4). Every passing line carries a value the KERNEL
 * produced, so a probe that skipped the behaviour cannot print it:
 *   A  a shared page written by the child reads 0x22 in the parent.
 *   B  a private page written by the child still reads 0x11 in the parent.
 *   C  256 cycles of (mmap, fork, child write+exit, wait, munmap), run once
 *      with a PRIVATE page (baseline: COW copy freed at child exit) and once
 *      with a SHARED page. The shared loop's SYS_SYSINFO.free_pages drop
 *      exceeds the private loop's by < 128; a per-cycle shared leak adds
 *      >= 256. A bare bound on the shared drop was measured too thin: fork
 *      churn alone dropped 96 (CI run 39177510529, DDR-1178 §4).
 *   F  the child munmaps the inherited shared region: rc 0 (its VMA table was
 *      copied at fork), and the parent's page survives (refcount, arm A).
 *   E  mprotect(RW) on a shared page, then fork: the child's 0x44 reaches the
 *      parent (PTE_SW_SHMEM survives vmm_protect_range).
 *   D  MAP_SHARED with an fd, MAP_SHARED|MAP_PRIVATE, MAP_SHARED|PROT_EXEC are
 *      each -EINVAL.
 * C runs only after A held: without a working shared map it is vacuous.
 */

#define SYS_EXIT     4
#define SYS_WRITE    6
#define SYS_MMAP    12
#define SYS_MUNMAP  13
#define SYS_FORK    15
#define SYS_WAIT4   16
#define SYS_SYSINFO 71
#define SYS_MPROTECT 97

#define PROT_RW      3
#define PROT_RWX     7
#define MAP_SHARED   0x01
#define MAP_PRIVATE  0x02
#define MAP_ANON     0x20
#define EINVAL       22

#define SHM_VA  0x8E00000000ul   /* inside [VMM_MMAP_BASE, VMM_MMAP_TOP) */
#define PRIV_VA 0x8E00001000ul
#define LOOP_VA 0x8E00010000ul
#define MPR_VA  0x8E00020000ul
#define CYCLES  256
#define LEAK_BOUND 128           /* half the mutant's minimum effect (DDR-1178 §4) */

struct sysinfo {
    char vendor[16]; char brand[64];
    unsigned cpu_count, feat_edx, feat_ecx, pad;
    unsigned long uptime_ticks, free_pages;
};

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}
static inline long nsi6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
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
static char *hex2(char *p, unsigned v) {
    const char *h = "0123456789abcdef";
    *p++ = '0'; *p++ = 'x'; *p++ = h[(v >> 4) & 15]; *p++ = h[v & 15];
    return p;
}
static char *str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static void line2(const char *a, long v) {
    char b[96], *p = str(b, a); p = dec(p, v); *p++ = '\n'; *p = 0; wr(b);
}
static __attribute__((noreturn)) void fail(const char *what, long v) {
    char b[96], *p = str(b, "PRADYOS_SHM FAIL "); p = str(p, what); *p++ = '=';
    p = dec(p, v); *p++ = '\n'; *p = 0; wr(b);
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

static long mmap6(long va, long prot, long flags, long fd) {
    return nsi6(SYS_MMAP, va, 4096, prot, flags, fd, 0);
}
static unsigned long free_pages(void) {
    struct sysinfo si;
    if (nsi(SYS_SYSINFO, (long)&si, 0, 0) != 0) fail("sysinfo", -1);
    return si.free_pages;
}
static int reap(long kid) {
    int st = 0;
    long r = nsi(SYS_WAIT4, kid, (long)&st, 0);
    if (r != kid) fail("wait4", r);
    return st;
}

static long loop_drop(long flags) {
    unsigned long f0 = free_pages();
    for (long i = 0; i < CYCLES; i++) {
        long r = mmap6((long)LOOP_VA, PROT_RW, flags, -1);
        if (r != (long)LOOP_VA) fail("loop_mmap", r);
        long k = nsi(SYS_FORK, 0, 0, 0);
        if (k < 0) fail("loop_fork", k);
        if (k == 0) {
            *(volatile unsigned char *)LOOP_VA = 0x33;
            nsi(SYS_EXIT, 0, 0, 0);
            for (;;) { }
        }
        reap(k);
        nsi(SYS_MUNMAP, (long)LOOP_VA, 4096, 0);
    }
    return (long)f0 - (long)free_pages();
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    long r = mmap6((long)SHM_VA, PROT_RW, MAP_SHARED | MAP_ANON, -1);
    if (r != (long)SHM_VA) fail("mmap", r);
    r = mmap6((long)PRIV_VA, PROT_RW, MAP_PRIVATE | MAP_ANON, -1);
    if (r != (long)PRIV_VA) fail("mmap_priv", r);
    volatile unsigned char *sh = (volatile unsigned char *)SHM_VA;
    volatile unsigned char *pv = (volatile unsigned char *)PRIV_VA;
    *sh = 0x11; *pv = 0x11;

    long kid = nsi(SYS_FORK, 0, 0, 0);
    if (kid < 0) fail("fork", kid);
    if (kid == 0) {
        *sh = 0x22; *pv = 0x22;
        long u = nsi(SYS_MUNMAP, (long)SHM_VA, 4096, 0);
        nsi(SYS_EXIT, u == 0 ? 0 : 7, 0, 0);
        for (;;) { }
    }
    int kst = reap(kid);

    char b[96], *p;
    unsigned a = *sh, pb = *pv;
    p = str(b, "PRADYOS_SHM_A shared="); p = hex2(p, a); *p++ = '\n'; *p = 0; wr(b);
    p = str(b, "PRADYOS_SHM_B private="); p = hex2(p, pb); *p++ = '\n'; *p = 0; wr(b);
    line2("PRADYOS_SHM_F child_munmap_ok=", kst == 0);
    nsi(SYS_MUNMAP, (long)SHM_VA, 4096, 0);
    nsi(SYS_MUNMAP, (long)PRIV_VA, 4096, 0);

    if (a == 0x22) {
        long drop_pv = loop_drop(MAP_PRIVATE | MAP_ANON);
        long drop_sh = loop_drop(MAP_SHARED | MAP_ANON);
        long drop = drop_sh - drop_pv;
        p = str(b, "PRADYOS_SHM_C cycles="); p = dec(p, CYCLES);
        p = str(p, " leaked_ok="); p = dec(p, drop < LEAK_BOUND);
        *p++ = '\n'; *p = 0; wr(b);
        line2("[shm] free_pages drop private=", drop_pv);
        line2("[shm] free_pages drop shared=", drop_sh);
    }

    if (a == 0x22) {
        r = mmap6((long)MPR_VA, PROT_RW, MAP_SHARED | MAP_ANON, -1);
        if (r != (long)MPR_VA) fail("mpr_mmap", r);
        volatile unsigned char *mp = (volatile unsigned char *)MPR_VA;
        *mp = 0x11;
        r = nsi(SYS_MPROTECT, (long)MPR_VA, 4096, PROT_RW);
        if (r != 0) fail("mprotect", r);
        long k = nsi(SYS_FORK, 0, 0, 0);
        if (k < 0) fail("mpr_fork", k);
        if (k == 0) {
            *mp = 0x44;
            nsi(SYS_EXIT, 0, 0, 0);
            for (;;) { }
        }
        reap(k);
        p = str(b, "PRADYOS_SHM_E mprotect_shared="); p = hex2(p, *mp); *p++ = '\n'; *p = 0; wr(b);
        nsi(SYS_MUNMAP, (long)MPR_VA, 4096, 0);
    }

    long rejects = 0;
    if (mmap6((long)SHM_VA, PROT_RW, MAP_SHARED, 0) == -EINVAL) rejects++;
    if (mmap6((long)SHM_VA, PROT_RW, MAP_SHARED | MAP_PRIVATE | MAP_ANON, -1) == -EINVAL) rejects++;
    if (mmap6((long)SHM_VA, PROT_RWX, MAP_SHARED | MAP_ANON, -1) == -EINVAL) rejects++;
    line2("PRADYOS_SHM_D rejects=", rejects);
    wr("PRADYOS_SHM_DONE\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
