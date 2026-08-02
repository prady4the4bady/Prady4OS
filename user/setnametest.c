/* user/setnametest.c — SYS_SETNAME self-rename probe (DDR-756).
 *
 * Self-contained round-trip: rename self to "KILROY", then walk SYS_GETPROCS to
 * find our own pid and confirm the process table now shows the new name. All-pass
 * -> PRADYOS_SETNAME_OK, else "SETNAME FAIL" + exit 1.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_GETPID   2
#define SYS_EXIT     4
#define SYS_WRITE    6
#define SYS_GETPROCS 67
#define SYS_SETNAME  75

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

struct procinfo {
    unsigned int pid, ppid, state, flags;
    char name[16];
    unsigned long long run_ticks, dispatches;
};

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn)) static void fail(const char *why) {
    wr("SETNAME FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

static int streq(const char *a, const char *b) {
    long i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == 0 && b[i] == 0;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    const char *newname = "KILROY";
    if (nsi(SYS_SETNAME, (long)newname, 0, 0) != 0)
        fail("SYS_SETNAME returned error");

    long mypid = nsi(SYS_GETPID, 0, 0, 0);

    /* Walk the process table and confirm our own entry carries the new name. */
    struct procinfo pi;
    int found = 0;
    for (long i = 0; ; i++) {
        if (nsi(SYS_GETPROCS, i, (long)&pi, 0) <= 0) break;
        if ((long)pi.pid == mypid) {
            if (!streq(pi.name, newname)) fail("name not updated in process table");
            found = 1;
            break;
        }
    }
    if (!found) fail("own pid not found in process table");

    wr("PRADYOS_SETNAME_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
