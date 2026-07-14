/* user/dmesgtest.c — SYS_DMESG kernel-log read-back probe (DDR-750).
 *
 * Self-contained + ring-size-independent: write a unique marker (which flows
 * through the kernel's kputc into the log ring), then SYS_DMESG the recent log
 * and confirm the marker is present. Found -> PRADYOS_DMESG_OK, else
 * "DMESG FAIL" + exit 1.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_WRITE  6
#define SYS_EXIT   4
#define SYS_DMESG 73

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
    wr("DMESG FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

/* Does haystack[0..hn) contain the NUL-terminated needle? */
static int contains(const char *hay, long hn, const char *needle) {
    long nn = slen(needle);
    if (nn == 0 || hn < nn) return 0;
    for (long i = 0; i + nn <= hn; i++) {
        long j = 0;
        while (j < nn && hay[i + j] == needle[j]) j++;
        if (j == nn) return 1;
    }
    return 0;
}

__attribute__((noreturn)) void _start(void) {
    /* A unique marker for THIS probe — deliberately not a sentinel any gate
     * greps directly (the gate keys on PRADYOS_DMESG_OK below). */
    const char *marker = "DMESGRINGMARKER-750";

    /* (1) Emit the marker; it flows console -> kputc -> the log ring. */
    wr(marker);
    wr("\n");

    /* (2) Read the recent kernel log back through the syscall. */
    char buf[4096];
    long n = nsi(SYS_DMESG, (long)buf, (long)sizeof buf, 0);
    if (n <= 0) fail("SYS_DMESG returned no data");

    /* (3) The marker we just wrote must be in the returned log. */
    if (!contains(buf, n, marker)) fail("marker not found in kernel log");

    wr("PRADYOS_DMESG_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
