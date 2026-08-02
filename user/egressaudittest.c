/* user/egressaudittest.c — R3 per-destination egress audit probe (DDR-801).
 *
 * Runs NON-sovereign with CAP_NET, so neither record it checks can come from
 * the DDR-800 sovereign-bypass path — otherwise the gate would pass on the
 * wrong evidence.
 *
 * Two connects, both asserted from the audit log:
 *   192.0.2.1:9999  — on the allowlist (kmain's DDR-734 self-test installs it)
 *                     -> AR_NET_CONNECT   carrying that destination
 *   192.0.2.1:9998  — SAME host, wrong port, so not on the allowlist
 *                     -> AR_CAP_DENIED    carrying that destination
 *
 * The second case is what makes this discriminating on the ENCODING as well as
 * the path: same host, different port, so an implementation that records the
 * host but drops the port emits two identical action_ids and cannot satisfy
 * both assertions.
 *
 * 192.0.2.x is TEST-NET-1 (RFC 5737) — nothing routes there in the guest. The
 * TCP outcome is irrelevant; the authority decision and its record happen
 * before any packet leaves.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_WRITE          6
#define SYS_EXIT           4
#define SYS_READ_AUDIT    37
#define SYS_SOCK_CONNECT  39

/* Must match enum aether_result / aether_action in kernel/aether/aether.h. */
#define AR_CAP_DENIED        7
#define AR_SOVEREIGN_BYPASS  9
#define AR_NET_CONNECT      10
#define ACTION_NET_CONNECT   4

#define HOST_BE      0xC0000201u      /* 192.0.2.1 */
#define PORT_ALLOWED 9999
#define PORT_DENIED  9998

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
    wr("EGRESSAUDIT FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

/* Is there a record with exactly this (action_type, action_id, result)? */
static int seen(const struct audit_entry *e, long n,
                unsigned long want_id, unsigned want_result) {
    for (long i = 0; i < n; i++)
        if (e[i].action_type == ACTION_NET_CONNECT &&
            e[i].action_id == want_id && e[i].result == want_result)
            return 1;
    return 0;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    (void)nsi(SYS_SOCK_CONNECT, (long)HOST_BE, PORT_ALLOWED, 0);
    (void)nsi(SYS_SOCK_CONNECT, (long)HOST_BE, PORT_DENIED, 0);

    struct audit_entry buf[128];
    long n = nsi(SYS_READ_AUDIT, (long)buf, 128, 0);
    if (n <= 0) fail("SYS_READ_AUDIT returned nothing");

    const unsigned long id_allowed =
        ((unsigned long)HOST_BE << 16) | (unsigned long)PORT_ALLOWED;
    const unsigned long id_denied =
        ((unsigned long)HOST_BE << 16) | (unsigned long)PORT_DENIED;

    if (!seen(buf, n, id_allowed, AR_NET_CONNECT))
        fail("no AR_NET_CONNECT record for the allowlisted destination");
    if (!seen(buf, n, id_denied, AR_CAP_DENIED))
        fail("no AR_CAP_DENIED record for the off-allowlist destination");

    /* Neither may have come from the operator-authority path: this probe is not
     * sovereign, so a bypass record here would mean the flag leaked. */
    if (seen(buf, n, id_allowed, AR_SOVEREIGN_BYPASS) ||
        seen(buf, n, id_denied, AR_SOVEREIGN_BYPASS))
        fail("a non-sovereign probe produced a sovereign-bypass record");

    wr("PRADYOS_EGRESS_AUDITED\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
