/* user/ipctest.c — the ring-3 IPC door, both sides of the gate (DDR-1033).
 *
 * ACTION_SEND_IPC was deferred by DDR-1017 because ipc_send/ipc_recv were
 * kernel-internal with no ring-3 door. The door is NSI 98/99; this exercises it.
 *
 * THE KERNEL SPAWNS THIS TWICE, once with the door granted and once without,
 * and that is not redundancy -- `is_ipc` is a PER-PROCESS flag, so one process
 * cannot exercise both the allowed and refused paths. Without the second spawn
 * the gate could be hardcoded open and every other arm would still pass. The
 * probe reports the return code of its first call either way, so the two runs
 * are told apart by the VALUE, not by which sentinels are missing.
 */

#include "uline.h"          /* DDR-1056: one write per measured line */

#define SYS_EXIT      4
#define SYS_WRITE     6
#define SYS_IPC_SEND 98
#define SYS_IPC_RECV 99

#define EPERM   1
#define EINVAL 22
#define SLOT    2                /* not slot 0: addressing another slot is the point */
#define BAD_SLOT 99

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

__attribute__((noreturn)) static void fail(const char *why, long v) {
    wr("IPCTEST FAIL: "); wr(why); wr(" rc="); wrdec(v); wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    unsigned long msg[4];
    msg[0] = 11; msg[1] = 22; msg[2] = 33; msg[3] = 44;

    /* ARM A / ARM B -- the same call, reported by value. rc=0 in the granted
     * process; rc=-EPERM in the un-granted one. */
    long rc = nsi(SYS_IPC_SEND, SLOT, (long)msg, 0);
    { uline u; ul_init(&u); ul_s(&u, "PRADYOS_IPC_GATE rc=");    /* DDR-1056 */
      ul_d(&u, rc); ul_s(&u, "\n"); wr(ul_end(&u)); }
    if (rc != 0) {
        if (rc != -EPERM) fail("refused, but not with EPERM", rc);
        nsi(SYS_EXIT, 0, 0, 0);            /* the un-granted run ends here */
        for (;;) { }
    }

    /* ARM D -- slot bounds. */
    long b = nsi(SYS_IPC_SEND, BAD_SLOT, (long)msg, 0);
    if (b != -EINVAL) fail("out-of-range slot was accepted", b);
    { uline u; ul_init(&u); ul_s(&u, "PRADYOS_IPC_SLOT rc=");    /* DDR-1056 */
      ul_d(&u, b); ul_s(&u, "\n"); wr(ul_end(&u)); }

    /* ARM C -- the payload round-trips. FIRST AND LAST word, because
     * IPC_MSG_WORDS is 4 and a copy that moved only msg[0] would otherwise read
     * as a success. */
    unsigned long out[4];
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
    long r2 = nsi(SYS_IPC_RECV, SLOT, (long)out, 0);
    if (r2 != 0) fail("recv", r2);
    if (out[0] != 11 || out[3] != 44) fail("payload did not round-trip", (long)out[3]);
    { uline u; ul_init(&u); ul_s(&u, "PRADYOS_IPC_RT w0=");      /* DDR-1056 */
      ul_d(&u, (long)out[0]); ul_s(&u, " w3="); ul_d(&u, (long)out[3]);
      ul_s(&u, "\n"); wr(ul_end(&u)); }

    wr("PRADYOS_IPC_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
