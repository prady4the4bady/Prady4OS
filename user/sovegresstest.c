/* user/sovegresstest.c — R1 sovereign-egress audit probe (DDR-800).
 *
 * Spawned SOVEREIGN with is_net = 0 (the exact shape DDR-800 is about: a thread
 * that has no CAP_NET and whose destination is not on the allowlist, yet is
 * allowed through because the operator flag covers it).
 *
 * Two assertions, and the second is the one that matters:
 *   1. the connect is still ALLOWED  — the exemption is deliberate and stays;
 *   2. it produced an audit record with AR_SOVEREIGN_BYPASS carrying the
 *      destination — the exemption is now RECORDED.
 *
 * An implementation that keeps the bypass and forgets the record passes (1) and
 * fails (2), which is precisely the defect DDR-800 closes.
 *
 * The destination is 198.51.100.7:4242 — TEST-NET-2 (RFC 5737). Nothing routes
 * there in the guest, so the connect cannot succeed at the TCP level; that is
 * irrelevant, because the authority decision and its audit record happen before
 * any packet is sent. The probe asserts on the RECORD, not on reachability.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_WRITE          6
#define SYS_EXIT           4
#define SYS_READ_AUDIT    37
#define SYS_SOCK_CONNECT  39

/* Must match enum aether_result / aether_action in kernel/aether/aether.h. */
#define AR_SOVEREIGN_BYPASS  9
#define ACTION_NET_CONNECT   4

#define DEST_HOST_BE  0xC6336407u      /* 198.51.100.7, big-endian as the NSI takes it */
#define DEST_PORT     4242

struct audit_entry {
    unsigned long timestamp;
    unsigned pid, action_type;
    unsigned long action_id;
    unsigned result, _pad;
};

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
    wr("SOVEGRESS FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    /* 1. The exemption must still work. -EPERM here would mean the bypass was
     *    closed rather than recorded, which DDR-800 explicitly rejected: an
     *    operator needs egress to diagnose the network. Any other result —
     *    a slot, or -EMFILE, or a TCP-level failure — means the AUTHORITY
     *    decision let it through, which is what is under test. */
    long rc = nsi(SYS_SOCK_CONNECT, (long)DEST_HOST_BE, DEST_PORT, 0);
    if (rc == -1L) fail("connect returned -EPERM; the exemption was closed");

    /* 2. ...and it must have left a record naming where it went. */
    struct audit_entry buf[64];
    long n = nsi(SYS_READ_AUDIT, (long)buf, 64, 0);
    if (n <= 0) fail("SYS_READ_AUDIT returned nothing");

    const unsigned long want_id =
        ((unsigned long)DEST_HOST_BE << 16) | (unsigned long)DEST_PORT;

    for (long i = 0; i < n; i++) {
        if (buf[i].result == AR_SOVEREIGN_BYPASS &&
            buf[i].action_type == ACTION_NET_CONNECT &&
            buf[i].action_id == want_id) {
            wr("PRADYOS_SOVEGRESS_AUDITED\n");
            nsi(SYS_EXIT, 0, 0, 0);
            for (;;) { }
        }
    }
    /* The bypass happened and nothing recorded it — the R1 defect exactly. */
    fail("no AR_SOVEREIGN_BYPASS record carrying the destination");
}
