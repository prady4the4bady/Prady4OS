/* user/sigsemtest.c — DDR-1174: POSIX signal semantics (C1.1).
 *
 * Six arms, each printing a value the KERNEL produced, so a probe that skipped
 * the behaviour cannot print the passing line:
 *   A  sigprocmask: a blocked SIGUSR1 does not run (pre=0), unblocking delivers
 *      it on that syscall's exit (post=1).
 *   B  SIGKILL is unmaskable: SIG_BLOCK of it reads back clear (kill=0).
 *   C  SA_SIGINFO: a child signals the parent; si_pid equals the child's pid,
 *      a value only the kernel's send-time record can supply.
 *   D  SIGCHLD: the child exit(42)s; the parent's handler sees status=42.
 *   E  EINTR: a pipe read with a live, silent writer is interrupted by a
 *      handled signal and returns EXACTLY -4 (not "< 0": DDR-1044).
 *   F  SA_RESTART: same, with SA_RESTART; the read restarts and returns the 2
 *      bytes written afterwards ("ok"), and the handler ran exactly once.
 *
 * Handler state lives in a page mapped at a fixed hint (no writable globals:
 * user.ld gives a probe one R+X segment). Every handler is SA_SIGINFO, so the
 * kernel moves RSP past the interrupted frame's red zone. A handler ends with
 * SYS_SIGRETURN; it never returns.
 */

#define SYS_GETPID       2
#define SYS_YIELD        3
#define SYS_EXIT         4
#define SYS_READ         5
#define SYS_WRITE        6
#define SYS_MMAP        12
#define SYS_FORK        15
#define SYS_WAIT4       16
#define SYS_PIPE        17
#define SYS_KILL        23
#define SYS_SIGRETURN   24
#define SYS_CLOCK       57
#define SYS_SIGPROCMASK 108
#define SYS_SIGACTION2  109

#define SIGKILL     9
#define SIGUSR1    10
#define SIGCHLD    17
#define SA_SIGINFO  0x00000004ul
#define SA_RESTART  0x10000000ul
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define STATE_VA 0x8E00000000ul  /* inside [VMM_MMAP_BASE, VMM_MMAP_TOP); TOP itself is refused -EINVAL */

struct k_sigaction { unsigned long handler, flags, mask; };
struct k_siginfo   { int signo, code, pid, status; };

struct state {
    volatile long usr1_runs;
    volatile long last_pid;
    volatile long chld_runs;
    volatile long chld_pid;
    volatile long chld_status;
    volatile long chld_code;
};
#define ST ((struct state *)STATE_VA)

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
static __attribute__((noreturn)) void die(const char *s) {
    wr("SIGSEM FAIL: "); wr(s); wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

static char *dec(char *p, long v) {
    if (v < 0) { *p++ = '-'; v = -v; }
    char t[20]; int n = 0;
    do { t[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}
static char *str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

/* Wall time, bounded two ways (DDR-1068/1090: an instruction count is a guess
 * about emulation speed; the iteration cap guarantees termination). */
static void waitsec(long secs) {
    long t0 = nsi(SYS_CLOCK, 0, 0, 0);
    for (long i = 0; i < 2000000; i++) {
        long w = nsi(SYS_CLOCK, 0, 0, 0) - t0;
        if (w < 0 || w >= secs) return;
        nsi(SYS_YIELD, 0, 0, 0);
    }
}

__attribute__((noreturn, force_align_arg_pointer))
static void on_usr1(long sig, struct k_siginfo *si) {
    (void)sig;
    ST->usr1_runs++;
    ST->last_pid = si ? si->pid : -1;
    nsi(SYS_SIGRETURN, 0, 0, 0);
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer))
static void on_chld(long sig, struct k_siginfo *si) {
    (void)sig;
    ST->chld_runs++;
    ST->chld_pid = si->pid;
    ST->chld_status = si->status;
    ST->chld_code = si->code;
    nsi(SYS_SIGRETURN, 0, 0, 0);
    for (;;) { }
}

static void install(int sig, void (*h)(long, struct k_siginfo *), unsigned long flags) {
    struct k_sigaction a = { (unsigned long)h, flags | SA_SIGINFO, 0 };
    long rc = nsi(SYS_SIGACTION2, sig, (long)&a, 0);
    if (rc != 0) {
        /* Print the value: "it failed" alone cannot tell -EFAULT from -ENOSYS. */
        char b[64], *p = str(b, "sigaction2 sig="); p = dec(p, sig);
        p = str(p, " act="); p = dec(p, (long)&a); p = str(p, " rc="); p = dec(p, rc);
        *p = 0;
        die(b);
    }
}

static long mask_op(long how, unsigned long set) {
    unsigned long old = 0;
    if (nsi(SYS_SIGPROCMASK, how, (long)&set, (long)&old) != 0)
        die("sigprocmask");
    return (long)old;
}

/* E/F: block in read() on a pipe whose writer (a child) waits, signals us,
 * waits, then writes "ok". Returns the read's result; *got gets the bytes. */
static long interrupted_read(long me, char *got) {
    int fds[2];
    if (nsi(SYS_PIPE, (long)fds, 0, 0) != 0) die("pipe");
    long kid = nsi(SYS_FORK, 0, 0, 0);
    if (kid < 0) die("fork");
    if (kid == 0) {
        waitsec(2);
        nsi(SYS_KILL, me, SIGUSR1, 0);
        waitsec(2);
        nsi(SYS_WRITE, fds[1], (long)"ok", 2);
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }
    long rc = nsi(SYS_READ, fds[0], (long)got, 2);
    long st = 0;
    nsi(SYS_WAIT4, kid, (long)&st, 0);
    return rc;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    long m = nsi6(SYS_MMAP, (long)STATE_VA, 4096, 3, 0x22, -1, 0);
    if (m != (long)STATE_VA) die("mmap");
    long me = nsi(SYS_GETPID, 0, 0, 0);
    char line[160]; char *p;

    install(SIGUSR1, on_usr1, 0);

    /* A: blocked -> not delivered; unblock -> delivered at that syscall's exit. */
    mask_op(SIG_BLOCK, 1ul << SIGUSR1);
    nsi(SYS_KILL, me, SIGUSR1, 0);
    long pre = ST->usr1_runs;
    mask_op(SIG_UNBLOCK, 1ul << SIGUSR1);
    long post = ST->usr1_runs;
    p = str(line, "PRADYOS_SIGSEM_A pre="); p = dec(p, pre);
    p = str(p, " post="); p = dec(p, post); *p++ = '\n';
    nsi(SYS_WRITE, 1, (long)line, p - line);

    /* B: SIGKILL cannot be blocked. */
    mask_op(SIG_BLOCK, 1ul << SIGKILL);
    long now = mask_op(SIG_BLOCK, 0);
    p = str(line, "PRADYOS_SIGSEM_B kill="); p = dec(p, (now >> SIGKILL) & 1);
    *p++ = '\n';
    nsi(SYS_WRITE, 1, (long)line, p - line);
    mask_op(SIG_SETMASK, 0);

    /* C + D: child signals the parent, then exits 42. */
    install(SIGCHLD, on_chld, 0);
    ST->usr1_runs = 0; ST->last_pid = 0;
    long kid = nsi(SYS_FORK, 0, 0, 0);
    if (kid < 0) die("fork");
    if (kid == 0) {
        nsi(SYS_KILL, me, SIGUSR1, 0);
        nsi(SYS_EXIT, 42, 0, 0);
        for (;;) { }
    }
    long st = 0;
    long reaped = nsi(SYS_WAIT4, kid, (long)&st, 0);
    for (int i = 0; i < 1000 && (!ST->usr1_runs || !ST->chld_runs); i++)
        nsi(SYS_YIELD, 0, 0, 0);
    p = str(line, "PRADYOS_SIGSEM_C sipid_ok="); p = dec(p, ST->last_pid == kid);
    p = str(p, " runs="); p = dec(p, ST->usr1_runs);
    *p++ = '\n';
    nsi(SYS_WRITE, 1, (long)line, p - line);
    p = str(line, "PRADYOS_SIGSEM_D chld_ok="); p = dec(p, ST->chld_pid == kid && reaped == kid);
    p = str(p, " status="); p = dec(p, ST->chld_status);
    p = str(p, " code="); p = dec(p, ST->chld_code);
    *p++ = '\n';
    nsi(SYS_WRITE, 1, (long)line, p - line);

    /* E: no SA_RESTART -> exactly -EINTR. */
    char got[4] = {0, 0, 0, 0};
    ST->usr1_runs = 0;
    long rce = interrupted_read(me, got);
    p = str(line, "PRADYOS_SIGSEM_E rc="); p = dec(p, rce);
    p = str(p, " runs="); p = dec(p, ST->usr1_runs); *p++ = '\n';
    nsi(SYS_WRITE, 1, (long)line, p - line);

    /* F: SA_RESTART -> the read restarts and gets the bytes. */
    install(SIGUSR1, on_usr1, SA_RESTART);
    ST->usr1_runs = 0;
    got[0] = got[1] = 0;
    long rcf = interrupted_read(me, got);
    p = str(line, "PRADYOS_SIGSEM_F rc="); p = dec(p, rcf);
    p = str(p, " data="); p = dec(p, got[0] == 'o' && got[1] == 'k');
    p = str(p, " runs="); p = dec(p, ST->usr1_runs); *p++ = '\n';
    nsi(SYS_WRITE, 1, (long)line, p - line);

    wr("PRADYOS_SIGSEM_DONE\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
