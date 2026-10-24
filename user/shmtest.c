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
#define SYS_YIELD     3
#define SYS_SHMGET  112          /* DDR-1178 Amendment 2 (P2) */
#define SYS_SHMAT   113
#define SYS_SHMDT   114
#define SYS_SHMCTL  115
#define IPC_PRIVATE  0
#define IPC_RMID     0
#define SHM_RDONLY   010000
#define EACCES       13
#define EIDRM        43
#define COMM_VA 0x8E00030000ul   /* MAP_SHARED page for arm I's early child */
#define SEG_PAGES   16
#define RECLAIM_MIN (SEG_PAGES - 4)

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
        /* The private loop is the baseline for fork churn. Unrelated frees
         * during it can make it NEGATIVE (CI run 39516558534: private=-225,
         * shared=0), and subtracting a negative baseline reported a leak
         * that the shared loop never had. A baseline below zero is taken as
         * zero: a real per-cycle shared leak (>= 256) still fails. */
        long drop = drop_sh - (drop_pv > 0 ? drop_pv : 0);
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

    /* ---- P2 (DDR-1178 Amendment 2): SysV shared memory, IPC_PRIVATE only ---- */
    /* I's child is forked BEFORE the segment exists, so its capability table
     * never receives the shmid: it can only learn the number, through C. */
    volatile long *C = (volatile long *)COMM_VA;
    if (mmap6((long)COMM_VA, PROT_RW, MAP_SHARED | MAP_ANON, -1) != (long)COMM_VA)
        fail("comm_mmap", -1);
    C[0] = C[1] = C[2] = C[3] = 0;
    long early = nsi(SYS_FORK, 0, 0, 0);
    if (early < 0) fail("i_fork", early);
    if (early == 0) {
        while (!C[0]) nsi(SYS_YIELD, 0, 0, 0);
        long forged = 0;
        if (nsi(SYS_SHMAT, C[1], 0, 0) >= 0) forged++;          /* the real id   */
        for (long gen = 1; gen <= 4; gen++)                       /* every guess   */
            for (long slot = 0; slot < 64; slot++)
                if (nsi(SYS_SHMAT, (gen << 6) | slot, 0, 0) >= 0) forged++;
        C[2] = forged;
        C[3] = 1;
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }

    /* G: a key is a namespace; only IPC_PRIVATE is accepted. */
    line2("PRADYOS_SHM_G key=", nsi(SYS_SHMGET, 0x1234, 4096, 0));

    /* H: shared through the segment across a fork made after shmget. */
    long id = nsi(SYS_SHMGET, IPC_PRIVATE, SEG_PAGES * 4096, 0);
    if (id < 0) fail("shmget", id);
    volatile unsigned char *hp = (volatile unsigned char *)nsi(SYS_SHMAT, id, 0, 0);
    if ((long)hp < 0) fail("shmat", (long)hp);
    hp[0] = 0x55;
    long hk = nsi(SYS_FORK, 0, 0, 0);
    if (hk < 0) fail("h_fork", hk);
    if (hk == 0) {
        volatile unsigned char *q = (volatile unsigned char *)nsi(SYS_SHMAT, id, 0, 0);
        if ((long)q < 0) nsi(SYS_EXIT, 2, 0, 0);
        q[0] = 0x66;
        nsi(SYS_SHMDT, (long)q, 0, 0);   /* J must not depend on the exit path */
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }
    reap(hk);
    {
        char b[96], *p = str(b, "PRADYOS_SHM_H shared="); p = hex2(p, hp[0]);
        *p++ = '\n'; *p = 0; wr(b);
    }

    /* I: the early child holds the number, not the capability. */
    C[1] = id;
    C[0] = 1;
    reap(early);
    line2("PRADYOS_SHM_I forged=", C[3] ? C[2] : -1);

    /* J: IPC_RMID marks; a new attach is -EIDRM; the last detach frees. */
    if (nsi(SYS_SHMCTL, id, IPC_RMID, 0) != 0) fail("rmid", -1);
    long eidrm = nsi(SYS_SHMAT, id, 0, 0);
    if (hp[0] != 0x66) fail("j_mapping_lost", hp[0]);
    unsigned long jf0 = free_pages();
    if (nsi(SYS_SHMDT, (long)hp, 0, 0) != 0) fail("shmdt", -1);
    long jgain = (long)free_pages() - (long)jf0;
    {
        char b[96], *p = str(b, "PRADYOS_SHM_J eidrm="); p = dec(p, eidrm);
        p = str(p, " freed_ok="); p = dec(p, jgain >= RECLAIM_MIN);
        *p++ = '\n'; *p = 0; wr(b);
    }

    /* K: SHM_RDONLY really is read-only, and mprotect cannot undo it. */
    long id2 = nsi(SYS_SHMGET, IPC_PRIVATE, 4096, 0);
    if (id2 < 0) fail("shmget2", id2);
    long kk = nsi(SYS_FORK, 0, 0, 0);
    if (kk < 0) fail("k_fork", kk);
    if (kk == 0) {
        volatile unsigned char *q = (volatile unsigned char *)nsi(SYS_SHMAT, id2, 0, SHM_RDONLY);
        if ((long)q < 0) nsi(SYS_EXIT, 3, 0, 0);
        q[0] = 0x77;                    /* must fault: the child dies here */
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }
    int kkst = reap(kk);
    long kro = nsi(SYS_SHMAT, id2, 0, SHM_RDONLY);
    if (kro < 0) fail("shmat_ro", kro);
    long kmp = nsi(SYS_MPROTECT, kro, 4096, PROT_RW);
    {
        char b[96], *p = str(b, "PRADYOS_SHM_K ro_faulted="); p = dec(p, kkst != 0 && kkst != 3);
        p = str(p, " ro_mprotect="); p = dec(p, kmp);
        *p++ = '\n'; *p = 0; wr(b);
    }
    nsi(SYS_SHMDT, kro, 0, 0);
    nsi(SYS_SHMCTL, id2, IPC_RMID, 0);

    /* L: a child that exits still attached is a detach (the exit path). */
    long id3 = nsi(SYS_SHMGET, IPC_PRIVATE, SEG_PAGES * 4096, 0);
    if (id3 < 0) fail("shmget3", id3);
    long la = nsi(SYS_SHMAT, id3, 0, 0);
    if (la < 0) fail("shmat3", la);
    long lk = nsi(SYS_FORK, 0, 0, 0);
    if (lk < 0) fail("l_fork", lk);
    if (lk == 0) {
        *(volatile unsigned char *)la = 0x11;   /* inherited attachment, no shmdt */
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }
    reap(lk);
    unsigned long lf0 = free_pages();
    nsi(SYS_SHMCTL, id3, IPC_RMID, 0);
    nsi(SYS_SHMDT, la, 0, 0);
    line2("PRADYOS_SHM_L nattch_fork=", (long)free_pages() - (long)lf0 >= RECLAIM_MIN);

    /* M: a segment nobody attached and nobody RMID'd goes when its last
     * holder (the child that created it) is gone. */
    unsigned long mf0 = free_pages();
    long mk = nsi(SYS_FORK, 0, 0, 0);
    if (mk < 0) fail("m_fork", mk);
    if (mk == 0) {
        long m = nsi(SYS_SHMGET, IPC_PRIVATE, SEG_PAGES * 4096, 0);
        nsi(SYS_EXIT, m < 0 ? 4 : 0, 0, 0);
        for (;;) { }
    }
    if (reap(mk) != 0) fail("m_child", -1);
    line2("PRADYOS_SHM_M reclaimed=", (long)mf0 - (long)free_pages() < RECLAIM_MIN);
    wr("PRADYOS_SHM_DONE\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
