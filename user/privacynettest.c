/* user/privacynettest.c — DDR-802 kernel privacy netfilter probe.
 *
 * Runs SOVEREIGN and WITH CAP_NET — the strongest credential the machine can
 * issue. That is the point: if privacy mode refuses this caller, it refuses
 * everyone. A probe that only proved an unprivileged process gets -EPERM would
 * prove nothing, because an unprivileged process is already refused.
 *
 * Two destinations, because they discriminate different failures:
 *
 *   192.0.2.1:9999  ON the allowlist. With privacy off this connect is allowed
 *                   by ordinary policy (AR_NET_CONNECT), so switching privacy
 *                   on flips a call that would otherwise unambiguously succeed.
 *                   Against an implementation that does nothing, the phase-2
 *                   assertion fails. This is the DDR-802 gate proper.
 *
 *   192.0.2.1:9998  SAME host, OFF the allowlist. Sovereign, so without DDR-802
 *                   this connect is permitted via the DDR-800 bypass and leaves
 *                   an AR_SOVEREIGN_BYPASS record. With DDR-802 ordered first,
 *                   that record must NOT exist — which is what proves the
 *                   privacy check runs AHEAD of the sovereign bypass rather
 *                   than merely existing somewhere in the function.
 *
 * Three phases (off -> on -> off) on the allowlisted destination. The third
 * phase catches a hook that latches on and never releases: a privacy mode that
 * cannot be switched off is a different defect, not a stricter version of this
 * one, and a two-phase probe would call it a pass.
 *
 * 192.0.2.x is TEST-NET-1 (RFC 5737) — nothing routes there in the guest. The
 * TCP outcome is irrelevant; the authority decision and its record both happen
 * before any packet leaves.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_WRITE          6
#define SYS_EXIT           4
#define SYS_SET_MODE      30
#define SYS_READ_AUDIT    37
#define SYS_SOCK_CONNECT  39

/* Must match enum aether_result / aether_action in kernel/aether/aether.h. */
#define AR_SOVEREIGN_BYPASS   9
#define AR_NET_CONNECT       10
#define AR_PRIVACY_BLOCKED   11
#define ACTION_NET_CONNECT    4

/* Must match the AETHER_MODE_* selectors in kernel/aether/aether.h. */
#define MODE_PRIVACY_ON   2
#define MODE_PRIVACY_OFF  3

#define EPERM 1

#define HOST_BE      0xC0000201u      /* 192.0.2.1 */
#define PORT_ALLOWED 9999
#define PORT_OFFLIST 9998

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
    wr("PRIVACYNET FAIL: ");
    wr(why);
    wr("\n");
    /* Leave privacy off on the way out — a probe that fails with egress still
     * disabled would take unrelated later gates down with it and disguise the
     * cause. Bounded: one call, no retry. */
    (void)nsi(SYS_SET_MODE, MODE_PRIVACY_OFF, 0, 0);
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
    const unsigned long id_allowed =
        ((unsigned long)HOST_BE << 16) | (unsigned long)PORT_ALLOWED;
    const unsigned long id_offlist =
        ((unsigned long)HOST_BE << 16) | (unsigned long)PORT_OFFLIST;

    /* Phase 1 — privacy OFF. The allowlisted destination must NOT be refused. */
    if (nsi(SYS_SOCK_CONNECT, (long)HOST_BE, PORT_ALLOWED, 0) == -EPERM)
        fail("allowlisted connect refused while privacy was OFF");

    /* Phase 2 — privacy ON. The same destination must now be refused, and the
     * off-allowlist one must be refused without taking the sovereign bypass. */
    if (nsi(SYS_SET_MODE, MODE_PRIVACY_ON, 0, 0) != 0)
        fail("SYS_SET_MODE(PRIVACY_ON) rejected");
    if (nsi(SYS_SOCK_CONNECT, (long)HOST_BE, PORT_ALLOWED, 0) != -EPERM)
        fail("allowlisted connect still permitted with privacy ON");
    if (nsi(SYS_SOCK_CONNECT, (long)HOST_BE, PORT_OFFLIST, 0) != -EPERM)
        fail("off-allowlist connect still permitted with privacy ON");

    /* Phase 3 — privacy OFF again. Catches a hook that latches on. */
    if (nsi(SYS_SET_MODE, MODE_PRIVACY_OFF, 0, 0) != 0)
        fail("SYS_SET_MODE(PRIVACY_OFF) rejected");
    if (nsi(SYS_SOCK_CONNECT, (long)HOST_BE, PORT_ALLOWED, 0) == -EPERM)
        fail("privacy mode latched on: still refusing after PRIVACY_OFF");

    wr("PRADYOS_PRIVACY_DENIED_OK\n");

    /* The audit trail has to carry the refusal, not just the return value. */
    struct audit_entry buf[256];
    long n = nsi(SYS_READ_AUDIT, (long)buf, 256, 0);
    if (n <= 0) fail("SYS_READ_AUDIT returned nothing");

    if (!seen(buf, n, id_allowed, AR_PRIVACY_BLOCKED))
        fail("no AR_PRIVACY_BLOCKED record for the allowlisted destination");
    if (!seen(buf, n, id_offlist, AR_PRIVACY_BLOCKED))
        fail("no AR_PRIVACY_BLOCKED record for the off-allowlist destination");

    /* Phases 1 and 3 were ordinary allowed connects; if that record is absent
     * the probe was never actually permitted and phase 2 proved nothing. */
    if (!seen(buf, n, id_allowed, AR_NET_CONNECT))
        fail("no AR_NET_CONNECT record from the privacy-OFF phases");

    /* THE ORDERING ASSERTION. This probe is sovereign and 9998 is off the
     * allowlist, so without DDR-802 ordered ahead of it the DDR-800 bypass
     * would have permitted that connect and left this record. Its absence is
     * what proves privacy is evaluated FIRST. */
    if (seen(buf, n, id_offlist, AR_SOVEREIGN_BYPASS)) {
        wr("PRADYOS_SOVEREIGN_BYPASSED\n");
        fail("sovereign bypass ran ahead of the privacy check");
    }

    wr("PRADYOS_PRIVACY_AUDIT_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
