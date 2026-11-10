/* user/mprotecttest.c — SYS_MPROTECT end to end (DDR-1031).
 *
 * Four arms, none implied by another:
 *
 *   A  mprotect(RW page -> read-only) returns 0
 *   B  a write to that page from a CHILD is actually refused by the CPU
 *   C  mprotect back to RW restores writability AND THE FRAME -- the value
 *      written before the RO transition must still be there
 *   D  PROT_WRITE|PROT_EXEC is refused (-EACCES): W^X
 *
 * WHY ARM B FORKS. A write to a read-only page is fatal, so a probe that did it
 * in-process would die and print nothing -- indistinguishable from a probe that
 * crashed for any other reason. The child takes the fault; the parent reads the
 * outcome out of wait4. A ring-3 fault ends in sched_exit(-1) (idt.c:703) and
 * wait4 copies the raw status (sys_wait.c), so:
 *
 *      status == -1  the CPU refused the write   -> enforced
 *      status ==  7  the write went through      -> NOT enforced
 *
 * and the child reaches its explicit exit(7) only if the store succeeded. Two
 * distinct values, so "enforced" is never inferred from silence.
 *
 * WHY THE ORDER IS mprotect-THEN-fork, AND WHY THAT IS NOT ACCIDENTAL. fork
 * marks a page copy-on-write ONLY IF IT IS WRITABLE (vmm_cow.c:87-92); a
 * read-only page is shared verbatim with no COW tag. Had this probe forked
 * first and protected afterwards, the child's store would have hit
 * vmm_cow_fault, been copied, and SUCCEEDED -- arm B would report "enforced"
 * on a kernel with no enforcement at all. Protecting first is what makes the
 * fault a genuine protection violation.
 *
 * ARMS F, G, H (DDR-1202, #112) -- ADDING WRITE MUST NOT REACH ANOTHER PROCESS.
 *
 *   F  a page made read-only and then shared by fork is shared AS IT IS (no
 *      COW tag). Whoever adds write back gets a private copy first:
 *        F1  the CHILD adds write and stores; the parent must keep its value
 *        F2  the PARENT adds write and stores; the child must keep its value
 *   G  write cannot be added to the vDSO page, a read-only view of a frame
 *      every process maps
 *   H  a MAP_SHARED page is NOT copied when write is added back: the store
 *      must reach the other process, because sharing is what was asked for
 *
 * F2 needs the child to read AFTER the parent's store. They share no private
 * memory (that is the point), so the turn is passed through one MAP_SHARED
 * page, which fork shares by design (DDR-1178).
 *
 * ARM C IS THE ONE THAT CATCHES A LOST FRAME. A vmm_protect_range that rebuilt
 * each PTE as `flags` alone -- dropping the frame, or dropping PTE_SW_SHARED /
 * PTE_SW_COW -- would still leave a page that maps and writes fine. Asserting
 * the ORIGINAL VALUE survives the RO round trip is what distinguishes "the
 * mapping still works" from "it is still the same mapping".
 */

#define SYS_YIELD     3
#define SYS_EXIT      4
#define SYS_WRITE     6
#define SYS_MMAP     12
#define SYS_FORK     15
#define SYS_WAIT4    16
#define SYS_MPROTECT 97          /* DDR-1031 */

#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define PROT_EXEC      0x4
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_ANONYMOUS  0x20

#define VDSO_USER_VA   0x00007FFFFFF00000ul   /* kernel/vdso/vdso_page.h */
#define ISO_A 0x11111111u        /* the value before the fork           */
#define ISO_B 0x22222222u        /* the value stored after adding write */

#define EACCES 13
#define SENTINEL 0x5A5AC0DEu
#define CHILD_WROTE 7            /* the child's exit code IF the store succeeds */

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* SYS_MMAP is the six-argument POSIX form (DDR-877). */
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

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

static void wrdec(long v) {
    char b[24];
    int i = 0, neg = 0;
    unsigned long u;
    if (v < 0) { neg = 1; u = (unsigned long)(-v); } else { u = (unsigned long)v; }
    if (!u) b[i++] = '0';
    while (u) { b[i++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) b[i++] = '-';
    while (i) { char c = b[--i]; nsi(SYS_WRITE, 1, (long)&c, 1); }
}

__attribute__((noreturn)) static void fail(const char *why, long v) {
    wr("MPROT FAIL: "); wr(why); wr(" rc="); wrdec(v); wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    /* One anonymous RW page. */
    long p = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p <= 0) fail("mmap", p);
    volatile unsigned *page = (volatile unsigned *)p;

    *page = SENTINEL;                       /* writable to begin with */
    if (*page != SENTINEL) fail("sentinel did not stick", (long)*page);

    /* A SECOND page, left WRITABLE across the fork below, so it becomes
     * copy-on-write. Arm E uses it. */
    long q = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q <= 0) fail("mmap cow page", q);
    volatile unsigned *cowp = (volatile unsigned *)q;
    *cowp = SENTINEL;

    /* ARM A -- drop to read-only. */
    long rc = nsi(SYS_MPROTECT, p, 4096, PROT_READ);
    if (rc != 0) fail("mprotect RO", rc);
    wr("PRADYOS_MPROT_RO rc=0\n");

    /* ARM B -- the child stores into it. Protected BEFORE the fork, so the page
     * is shared read-only with no COW tag and the store is a real violation. */
    long kid = nsi(SYS_FORK, 0, 0, 0);
    if (kid < 0) fail("fork", kid);
    if (kid == 0) {
        *page = 0xDEADBEEFu;                /* expected to fault and never return */
        nsi(SYS_EXIT, CHILD_WROTE, 0, 0);   /* reached only if it did NOT fault */
        for (;;) { }
    }
    int st = 0;
    long got = nsi(SYS_WAIT4, kid, (long)&st, 0);
    if (got != kid) fail("wait4", got);
    if (st == CHILD_WROTE) fail("child wrote through a read-only page", st);
    if (st != -1) fail("child died for some other reason", st);
    wr("PRADYOS_MPROT_ENFORCED st="); wrdec(st); wr("\n");

    /* ARM C -- restore write, and the ORIGINAL VALUE must still be there. */
    rc = nsi(SYS_MPROTECT, p, 4096, PROT_READ | PROT_WRITE);
    if (rc != 0) fail("mprotect RW", rc);
    if (*page != SENTINEL) fail("frame lost across the RO round trip", (long)*page);
    *page = SENTINEL ^ 0xFFFFu;             /* and it is genuinely writable again */
    if (*page != (SENTINEL ^ 0xFFFFu)) fail("still not writable", (long)*page);
    wr("PRADYOS_MPROT_RESTORED rc=0 val="); wrdec((long)SENTINEL); wr("\n");

    /* ARM D -- W^X. */
    rc = nsi(SYS_MPROTECT, p, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (rc != -EACCES) fail("W^X not enforced", rc);
    wr("PRADYOS_MPROT_WX rc="); wrdec(rc); wr("\n");

    /* ARM E -- the PTE_SW_COW tag SURVIVES a protection change.
     *
     * `q` was writable across the fork, so fork downgraded it to read-only and
     * tagged it COW in both address spaces (vmm_cow.c:87-92). Removing write
     * from a COW page is harmless and allowed; ADDING write is refused, because
     * the hardware RO bit is copy-on-write's trigger (DDR-1031 §3b).
     *
     * So the two calls below discriminate exactly the bit this arm exists for:
     * if vmm_protect_range had dropped PTE_SW_COW while rewriting the PTE, the
     * second call would come back 0 instead of -EACCES -- and the page would
     * then be writable with no copy, silently corrupting a frame the child's
     * address space still points at. This is the only arm that can see it: the
     * other four never touch a shared page. */
    rc = nsi(SYS_MPROTECT, q, 4096, PROT_READ);
    if (rc != 0) fail("mprotect RO on a COW page should be allowed", rc);
    rc = nsi(SYS_MPROTECT, q, 4096, PROT_READ | PROT_WRITE);
    if (rc != -EACCES) fail("COW tag lost across mprotect", rc);
    wr("PRADYOS_MPROT_COW rc="); wrdec(rc); wr("\n");

    /* ---- DDR-1202 (#112) ------------------------------------------------- */
    long ctl_va = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ctl_va <= 0) fail("mmap ctl", ctl_va);
    volatile unsigned *ctl = (volatile unsigned *)ctl_va;   /* [0] = F2's turn */
    ctl[0] = 0;

    /* ARM F1 -- the child adds write. */
    long f1 = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (f1 <= 0) fail("mmap f1", f1);
    volatile unsigned *pf1 = (volatile unsigned *)f1;
    *pf1 = ISO_A;
    rc = nsi(SYS_MPROTECT, f1, 4096, PROT_READ);
    if (rc != 0) fail("f1 mprotect RO", rc);
    kid = nsi(SYS_FORK, 0, 0, 0);
    if (kid < 0) fail("f1 fork", kid);
    if (kid == 0) {
        if (nsi(SYS_MPROTECT, f1, 4096, PROT_READ | PROT_WRITE) != 0)
            nsi(SYS_EXIT, 22, 0, 0);
        *pf1 = ISO_B;
        nsi(SYS_EXIT, (*pf1 == ISO_B) ? 21 : 23, 0, 0);
        for (;;) { }
    }
    st = 0;
    got = nsi(SYS_WAIT4, kid, (long)&st, 0);
    if (got != kid) fail("f1 wait4", got);
    if (st != 21) fail("f1 child could not add write and store", st);
    int child_up = (*pf1 == ISO_A);          /* the parent's page is untouched */

    /* ARM F2 -- the parent adds write while the child still maps the page. */
    long f2 = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (f2 <= 0) fail("mmap f2", f2);
    volatile unsigned *pf2 = (volatile unsigned *)f2;
    *pf2 = ISO_A;
    rc = nsi(SYS_MPROTECT, f2, 4096, PROT_READ);
    if (rc != 0) fail("f2 mprotect RO", rc);
    kid = nsi(SYS_FORK, 0, 0, 0);
    if (kid < 0) fail("f2 fork", kid);
    if (kid == 0) {
        while (ctl[0] == 0)                  /* wait for the parent's store */
            nsi(SYS_YIELD, 0, 0, 0);
        nsi(SYS_EXIT, (*pf2 == ISO_A) ? 31 : 32, 0, 0);
        for (;;) { }
    }
    rc = nsi(SYS_MPROTECT, f2, 4096, PROT_READ | PROT_WRITE);
    if (rc != 0) fail("f2 parent mprotect RW", rc);
    *pf2 = ISO_B;
    if (*pf2 != ISO_B) fail("f2 parent store did not stick", (long)*pf2);
    ctl[0] = 1;
    st = 0;
    got = nsi(SYS_WAIT4, kid, (long)&st, 0);
    if (got != kid) fail("f2 wait4", got);
    if (st != 31 && st != 32) fail("f2 child died for some other reason", st);
    int parent_up = (st == 31);
    wr("PRADYOS_MPROT_ISO child_up="); wrdec(child_up);
    wr(" parent_up="); wrdec(parent_up); wr("\n");

    /* ARM G -- the vDSO page cannot gain write. Nothing is stored into it
     * here: on a kernel that grants it, the page belongs to every process. */
    rc = nsi(SYS_MPROTECT, (long)VDSO_USER_VA, 4096, PROT_READ | PROT_WRITE);
    wr("PRADYOS_MPROT_VIEW rc="); wrdec(rc); wr("\n");

    /* ARM H -- a MAP_SHARED page stays shared when write is added back. */
    long h = nsi6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (h <= 0) fail("mmap h", h);
    volatile unsigned *ph = (volatile unsigned *)h;
    *ph = ISO_A;
    rc = nsi(SYS_MPROTECT, h, 4096, PROT_READ);
    if (rc != 0) fail("h mprotect RO", rc);
    kid = nsi(SYS_FORK, 0, 0, 0);
    if (kid < 0) fail("h fork", kid);
    if (kid == 0) {
        if (nsi(SYS_MPROTECT, h, 4096, PROT_READ | PROT_WRITE) != 0)
            nsi(SYS_EXIT, 42, 0, 0);
        *ph = ISO_B;
        nsi(SYS_EXIT, 41, 0, 0);
        for (;;) { }
    }
    st = 0;
    got = nsi(SYS_WAIT4, kid, (long)&st, 0);
    if (got != kid) fail("h wait4", got);
    if (st != 41) fail("h child could not add write to a shared page", st);
    wr("PRADYOS_MPROT_SHKEEP shared="); wrdec((*ph == ISO_B) ? 1 : 0); wr("\n");

    wr("PRADYOS_MPROT_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
