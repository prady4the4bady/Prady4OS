/* user/capnettest.c — CAP_NET socket-authority probe (DDR-731).
 *
 * Runs CAP-less and proves the socket NSI is gated:
 *   1. SYS_SOCK_CONNECT without CAP_NET -> exactly -EPERM (not a slot, not
 *      -EMFILE — the authority check fires before slot allocation)
 *                                             -> CAPNET_CONNECT_DENIED
 *   2. SYS_SOCK_WRITE / SYS_SOCK_CLOSE on slot 0 (never ours) -> -EPERM
 *      (per-slot ownership: a process cannot touch another's connection)
 *                                             -> CAPNET_SLOT_DENIED
 * Both -> PRADYOS_CAPNET_OK; anything else prints CAPNET FAIL and exits 1.
 *
 * Freestanding (no libc, user.ld, no writable globals) — kernel-image budget.
 */

#define SYS_WRITE         6
#define SYS_EXIT          4
#define SYS_SOCK_CONNECT  39
#define SYS_SOCK_WRITE    40
#define SYS_SOCK_CLOSE    42

#define EPERM 1

static inline long nsi4(long n, long a1, long a2, long a3, long a4) {
    long r;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}
static inline long nsi(long n, long a1, long a2, long a3) { return nsi4(n, a1, a2, a3, 0); }

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn)) static void die(const char *why) {
    wr("CAPNET FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn)) void _start(void) {
    /* 1. No CAP_NET: connect must be refused with -EPERM specifically. */
    long r = nsi(SYS_SOCK_CONNECT, 0x0A00020FL /* 10.0.2.15 be */, 80, 0);
    if (r != -EPERM) die("connect not EPERM");
    wr("CAPNET_CONNECT_DENIED\n");

    /* 2. Slot ownership: slot 0 was never connected by us — write and close
     * must be refused before touching the proxy state. */
    char b = 'x';
    if (nsi(SYS_SOCK_WRITE, 0, (long)&b, 1) != -EPERM) die("write not EPERM");
    if (nsi(SYS_SOCK_CLOSE, 0, 0, 0) != -EPERM) die("close not EPERM");
    wr("CAPNET_SLOT_DENIED\n");

    wr("PRADYOS_CAPNET_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
