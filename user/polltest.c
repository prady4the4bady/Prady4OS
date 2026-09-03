/* user/polltest.c — POSIX poll() (DDR-1037, NSI 102).
 *
 * The probe REPORTS and the gate JUDGES: every arm prints its numbers before
 * any exit, because a fail() before the print silently removes an arm.
 *
 * NOTE the arm-E design. A poll() that ignores its timeout entirely and returns
 * immediately would satisfy "returned 0" — so arm E stamps SYS_CLOCK either side
 * and reports the elapsed seconds. The arm is a measurement, not a restatement
 * of the return value (DDR-1037 §5) — and it caught its OWN first draft, which
 * differenced two SYS_TIME return codes and so measured nothing.
 */

#include "uline.h"          /* DDR-1056: one write per measured line */

#define SYS_EXIT     4
#define SYS_WRITE    6
#define SYS_OPEN     7
#define SYS_PIPE    17
/* SYS_CLOCK (57) returns seconds-since-midnight AS A VALUE. NOT SYS_TIME (72),
 * which takes a struct rtc_time* out-pointer and returns 0/-EFAULT — differencing
 * two of those measures nothing, and the first draft of this probe did exactly
 * that and reported waited=0 on a poll() that may well have waited correctly.
 * One-second resolution, which is enough for a 2 s wait (DDR-1029 used the same
 * clock and recorded its coarseness). */
#define SYS_CLOCK   57
#define SYS_POLL   102

#define POLLIN     0x001
#define POLLOUT    0x004
#define POLLNVAL   0x020

struct pollfd { int fd; short events; short revents; } __attribute__((packed));

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
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

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    int pfd[2];
    long rc = nsi(SYS_PIPE, (long)pfd, 0, 0);
    if (rc != 0) {
        wr("POLLTEST FAIL: pipe rc="); wrdec(rc); wr("\n");
        nsi(SYS_EXIT, 1, 0, 0);
        for (;;) { }
    }

    struct pollfd p[2];

    /* ---- ARM A: empty pipe, timeout 0 -> nothing ready ------------------- */
    p[0].fd = pfd[0]; p[0].events = POLLIN; p[0].revents = 0;
    long n = nsi(SYS_POLL, (long)p, 1, 0);
    { uline u; ul_init(&u); ul_s(&u, "PRADYOS_POLL_EMPTY n=");   /* DDR-1056 */
      ul_d(&u, n); ul_s(&u, "\n"); wr(ul_end(&u)); }

    /* ---- ARM B: after a write, the read end is POLLIN --------------------- */
    nsi(SYS_WRITE, pfd[1], (long)"x", 1);
    p[0].fd = pfd[0]; p[0].events = POLLIN; p[0].revents = 0;
    n = nsi(SYS_POLL, (long)p, 1, 0);
    { uline u; ul_init(&u); ul_s(&u, "PRADYOS_POLL_IN n=");      /* DDR-1056 */
      ul_d(&u, n); ul_s(&u, " rev="); ul_d(&u, (long)p[0].revents);
      ul_s(&u, "\n"); wr(ul_end(&u)); }

    /* ---- ARM C: a bad fd is POLLNVAL, not silence ------------------------- */
    p[0].fd = 4242; p[0].events = POLLIN; p[0].revents = 0;
    n = nsi(SYS_POLL, (long)p, 1, 0);
    { uline u; ul_init(&u); ul_s(&u, "PRADYOS_POLL_NVAL n=");    /* DDR-1056 */
      ul_d(&u, n); ul_s(&u, " rev="); ul_d(&u, (long)p[0].revents);
      ul_s(&u, "\n"); wr(ul_end(&u)); }

    /* ---- ARM D: a regular file is ALWAYS ready (POSIX) --------------------
     * The pre-DDR-1037 predicate returned 0 here, so this arm is a correctness
     * check, not a feature check. */
    long ffd = nsi(SYS_OPEN, (long)"/HELLO.TXT", 0, 0);
    if (ffd >= 0) {
        p[0].fd = (int)ffd; p[0].events = POLLIN | POLLOUT; p[0].revents = 0;
        n = nsi(SYS_POLL, (long)p, 1, 0);
        { uline u; ul_init(&u); ul_s(&u, "PRADYOS_POLL_FILE n="); /* DDR-1056 */
          ul_d(&u, n); ul_s(&u, " rev="); ul_d(&u, (long)p[0].revents);
          ul_s(&u, "\n"); wr(ul_end(&u)); }
    } else {
        { uline u; ul_init(&u); ul_s(&u, "PRADYOS_POLL_FILE open_rc=");
          ul_d(&u, ffd); ul_s(&u, "\n"); wr(ul_end(&u)); }
    }

    /* ---- ARM E: a positive timeout ACTUALLY ELAPSES -----------------------
     * Measured, not inferred: a poll() that ignored its timeout would also
     * return 0 here, so the arm asserts that time passed. */
    long t0 = nsi(SYS_CLOCK, 0, 0, 0);
    p[0].fd = pfd[0]; p[0].events = POLLOUT; p[0].revents = 0;   /* read end never POLLOUT */
    n = nsi(SYS_POLL, (long)p, 1, 2000);                          /* 2 s */
    long t1 = nsi(SYS_CLOCK, 0, 0, 0);
    { uline u; ul_init(&u); ul_s(&u, "PRADYOS_POLL_TMO n=");     /* DDR-1056 */
      ul_d(&u, n); ul_s(&u, " waited="); ul_d(&u, t1 - t0);
      ul_s(&u, "\n"); wr(ul_end(&u)); }

    wr("PRADYOS_POLL_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
