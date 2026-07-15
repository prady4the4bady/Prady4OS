/* user/killtest.c — process-signaling probe (DDR-755).
 *
 * Proves one ring-3 process can terminate another end-to-end: fork a child that
 * spins forever (no exit path of its own), then SYS_KILL it with SIGKILL and
 * SYS_WAIT4 it. Reaching past wait4 is the proof — the child cannot exit on its
 * own, so wait4 only returns if the signal terminated it. Prints
 * PRADYOS_KILL_OK; a fork/kill failure prints "KILL FAIL" + exit 1. If the kill
 * path were broken, wait4 would block forever (clean timeout, never a false pass).
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_EXIT   4
#define SYS_WRITE  6
#define SYS_YIELD  3
#define SYS_FORK   15
#define SYS_WAIT4  16
#define SYS_KILL   23
#define SIGKILL    9

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn)) static void fail(const char *why) {
    wr("KILL FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn)) void _start(void) {
    long pid = nsi(SYS_FORK, 0, 0, 0);
    if (pid < 0)
        fail("fork failed");
    if (pid == 0) {
        /* Child: spin in ring 3 forever — no exit path. Only a signal ends it. */
        for (;;)
            __asm__ volatile("pause");
    }

    /* Parent: let the child get scheduled, then kill and reap it. */
    for (int i = 0; i < 8; i++)
        nsi(SYS_YIELD, 0, 0, 0);
    if (nsi(SYS_KILL, pid, SIGKILL, 0) != 0)
        fail("kill returned error");

    int st = 0;
    long r = nsi(SYS_WAIT4, pid, (long)&st, 0);   /* blocks until the child dies */
    if (r != pid)
        fail("wait4 did not reap the killed child");

    /* Reached only because the signal terminated the otherwise-immortal child. */
    wr("PRADYOS_KILL_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
