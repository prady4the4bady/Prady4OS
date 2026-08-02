/* user/timetest.c — SYS_TIME wall-clock probe (DDR-749).
 *
 * Reads the broken-down RTC time, prints TIME YYYY-MM-DD HH:MM:SS for
 * visibility, and range-validates each field (exact values are host-provided so
 * cannot be asserted, but ranges always hold). All-pass -> PRADYOS_TIME_OK,
 * else "TIME FAIL" + exit 1.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_WRITE  6
#define SYS_EXIT   4
#define SYS_TIME  72

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* Mirror of the kernel struct rtc_time (kernel/drivers/rtc/rtc.h). */
struct rtc_time {
    unsigned short year;
    unsigned char  month, day, hour, minute, second;
};

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn)) static void fail(const char *why) {
    wr("TIME FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

/* Append `v` as a zero-padded `width`-digit decimal into buf at *pos. */
static void put_num(char *buf, int *pos, unsigned v, int width) {
    char tmp[8];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 8);
    while (n < width) tmp[n++] = '0';
    while (n > 0) buf[(*pos)++] = tmp[--n];
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    struct rtc_time t;
    if (nsi(SYS_TIME, (long)&t, 0, 0) != 0)
        fail("syscall returned nonzero");

    /* Format "TIME YYYY-MM-DD HH:MM:SS\n". */
    char b[48];
    int p = 0;
    const char *pfx = "TIME ";
    for (int i = 0; pfx[i]; i++) b[p++] = pfx[i];
    put_num(b, &p, t.year, 4);   b[p++] = '-';
    put_num(b, &p, t.month, 2);  b[p++] = '-';
    put_num(b, &p, t.day, 2);    b[p++] = ' ';
    put_num(b, &p, t.hour, 2);   b[p++] = ':';
    put_num(b, &p, t.minute, 2); b[p++] = ':';
    put_num(b, &p, t.second, 2); b[p++] = '\n';
    nsi(SYS_WRITE, 1, (long)b, p);

    if (t.year < 2020 || t.year >= 2100) fail("year out of range");
    if (t.month < 1 || t.month > 12)     fail("month out of range");
    if (t.day < 1 || t.day > 31)         fail("day out of range");
    if (t.hour > 23)                     fail("hour out of range");
    if (t.minute > 59)                   fail("minute out of range");
    if (t.second > 59)                   fail("second out of range");

    wr("PRADYOS_TIME_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
