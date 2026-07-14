/* user/sysinfotest.c — SYS_SYSINFO probe (DDR-748).
 *
 * Reads the system-info struct, echoes the CPU vendor/brand strings to the
 * console for visibility, and validates the numeric fields. On all-pass prints
 * PRADYOS_SYSINFO_OK; any bad field -> "SYSINFO FAIL" + exit 1.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_WRITE    6
#define SYS_EXIT     4
#define SYS_SYSINFO 71

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* Mirror of the kernel struct sysinfo (sys_proc.c). */
struct sysinfo {
    char     vendor[16];
    char     brand[64];
    unsigned cpu_count;
    unsigned feat_edx;
    unsigned feat_ecx;
    unsigned _pad;
    unsigned long long uptime_ticks;
    unsigned long long free_pages;
};

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn)) static void fail(const char *why) {
    wr("SYSINFO FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn)) void _start(void) {
    struct sysinfo si;
    if (nsi(SYS_SYSINFO, (long)&si, 0, 0) != 0)
        fail("syscall returned nonzero");

    /* Echo the identity strings (NUL-terminated by the kernel). */
    wr("SYSINFO vendor=");
    wr(si.vendor);
    wr(" brand=");
    wr(si.brand[0] ? si.brand : "(none)");
    wr("\n");

    if (si.cpu_count < 1)   fail("cpu_count < 1");
    if (si.vendor[0] == 0)  fail("empty vendor");
    if (si.brand[0] == 0)   fail("empty brand");
    if (si.free_pages == 0) fail("free_pages == 0");

    wr("PRADYOS_SYSINFO_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
