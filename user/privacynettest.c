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
 * PHASE 4 (DDR-1070) -- EGRESS ON AN ALREADY-OPEN SOCKET.
 *
 * Phases 1-3 all call SYS_SOCK_CONNECT, so their coverage is *connect* while
 * the feature's claim is *nothing leaves*. Privacy mode was checked only in
 * sys_sock_connect, so a socket opened before the operator switched it on kept
 * sending and receiving; every arm above is live and none of them could see it.
 *
 * THE OBVIOUS ARM IS VACUOUS, and it is why phase 4 changes destination.
 * Nothing routes to 192.0.2.1, so that connect never leaves PS_CONNECTING and a
 * write returns PSOCK_STALE -> -EBADF *whether or not the check exists*: an arm
 * asserting "the write failed" passes on a kernel with no privacy check on the
 * write path at all. So phase 4 uses 127.0.0.1:8007 -- the in-kernel TCP echo
 * server net_init() binds (lwip_port.c) and the same endpoint nethammer drives
 * at 20,000 connects with conn_err=0, so a loopback connect provably reaches
 * PS_OPEN -- it PROVES the socket is live by writing and reading the echo back
 * (an echo is the one thing this probe cannot manufacture), and only then
 * asserts the EXACT errno -EPERM, never merely "< 0".
 *
 * 4d re-enables egress and uses the SAME handle again. Without it, "privacy
 * refused the write" and "the write path is broken" are the same observation.
 *
 * The connection is NOT torn down by privacy mode and 4d is what says so:
 * DDR-1070 sec.5.3 refuses I/O reversibly rather than destroying state, because
 * phase 3's own rule is that privacy mode must be releasable.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld).
 */

#define SYS_WRITE          6
#define SYS_EXIT           4
#define SYS_SET_MODE      30
#define SYS_READ_AUDIT    37
#define SYS_SOCK_CONNECT  39
#define SYS_SOCK_WRITE    40
#define SYS_SOCK_READ     41
#define SYS_SOCK_CLOSE    42
#define SYS_YIELD          3

/* Must match enum aether_result / aether_action in kernel/aether/aether.h. */
#define AR_SOVEREIGN_BYPASS   9
#define AR_NET_CONNECT       10
#define AR_PRIVACY_BLOCKED   11
#define ACTION_NET_CONNECT    4
#define ACTION_NET_EGRESS    13        /* DDR-1070 */

/* Must match the AETHER_MODE_* selectors in kernel/aether/aether.h. */
#define MODE_PRIVACY_ON   2
#define MODE_PRIVACY_OFF  3

#define EPERM 1

#define HOST_BE      0xC0000201u      /* 192.0.2.1 */
#define PORT_ALLOWED 9999
#define PORT_OFFLIST 9998

/* DDR-1070 phase 4: loopback to the in-kernel echo server, the one destination
 * in this guest that reaches PS_OPEN. main.c seeds the allowlist row in the
 * same gated block that spawns this probe -- NOT for nethammer's reason (that
 * probe is not sovereign; this one is, so the DDR-800 bypass would let the
 * connect through regardless) but so phase 4 opens its socket through the
 * ORDINARY policy-permitted path an agent would take, rather than through the
 * operator bypass. Egress on a socket no ordinary agent could have opened would
 * be testing the wrong connection. */
#define LO_HOST_BE   0x7F000001u      /* 127.0.0.1 */
#define LO_PORT      8007
#define LO_TRIES     4000             /* bounded: one write attempt per yield */
#define LO_READ_MS   2000
#define LO_READ_TRIES 3               /* bounded; the echo is local and immediate */

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

/* SYS_SOCK_READ carries timeout_ms in a4, which the 3-arg form cannot reach. */
static inline long nsi4(long n, long a1, long a2, long a3, long a4) {
    long r;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
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
static int seen_t(const struct audit_entry *e, long n, unsigned want_type,
                  unsigned long want_id, unsigned want_result) {
    for (long i = 0; i < n; i++)
        if (e[i].action_type == want_type &&
            e[i].action_id == want_id && e[i].result == want_result)
            return 1;
    return 0;
}
static int seen(const struct audit_entry *e, long n,
                unsigned long want_id, unsigned want_result) {
    return seen_t(e, n, ACTION_NET_CONNECT, want_id, want_result);
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

    /* ---- Phase 4 (DDR-1070) -- egress on an ALREADY-OPEN socket ------------ */
    const unsigned long id_lo =
        ((unsigned long)LO_HOST_BE << 16) | (unsigned long)LO_PORT;
    char txbuf[4], rxbuf[4];
    txbuf[0] = 'p'; txbuf[1] = 'i'; txbuf[2] = 'n'; txbuf[3] = 'g';

    long h = nsi(SYS_SOCK_CONNECT, (long)LO_HOST_BE, LO_PORT, 0);
    if (h < 0)
        fail("phase 4: loopback connect failed");

    /* 4a -- PROVE the socket is live before asserting anything about refusing
     * it. tcp_connect only enqueues the SYN, so the handshake completes on a
     * later tick and an immediate write returns PSOCK_STALE -> -EBADF; that is
     * PS_CONNECTING, not a failure. Bounded retry, yielding between attempts. */
    long n4 = -1;
    for (long i = 0; i < LO_TRIES; i++) {
        n4 = nsi(SYS_SOCK_WRITE, h, (long)txbuf, 4);
        if (n4 == 4) break;
        (void)nsi(SYS_YIELD, 0, 0, 0);
    }
    if (n4 != 4)
        fail("phase 4a: loopback socket never opened (write never returned 4)");

    /* The echo is the half this probe cannot manufacture: it proves the bytes
     * crossed the stack and came back, i.e. that 4b is refusing a connection
     * that was really carrying traffic. */
    rxbuf[0] = rxbuf[1] = rxbuf[2] = rxbuf[3] = 0;
    long rn = 0;
    for (long i = 0; i < LO_READ_TRIES && rn <= 0; i++)
        rn = nsi4(SYS_SOCK_READ, h, (long)rxbuf, 4, LO_READ_MS);
    if (rn != 4 || rxbuf[0] != 'p' || rxbuf[1] != 'i' ||
        rxbuf[2] != 'n' || rxbuf[3] != 'g')
        fail("phase 4a: no echo back, the socket is not carrying data");

    /* 4b/4c -- privacy ON. Both directions, on the SAME live handle, and the
     * EXACT errno: -EBADF is also negative, so "< 0" would pass on a kernel
     * with no check at all. On the pre-fix kernel 4b returns 4 and the bytes
     * go out. */
    if (nsi(SYS_SET_MODE, MODE_PRIVACY_ON, 0, 0) != 0)
        fail("phase 4b: SYS_SET_MODE(PRIVACY_ON) rejected");
    if (nsi(SYS_SOCK_WRITE, h, (long)txbuf, 4) != -EPERM)
        fail("phase 4b: write on an OPEN socket still permitted with privacy ON");
    if (nsi4(SYS_SOCK_READ, h, (long)rxbuf, 4, LO_READ_MS) != -EPERM)
        fail("phase 4c: read on an OPEN socket still permitted with privacy ON");

    /* 4d -- release, on the SAME handle. Separates "privacy refused it" from
     * "the I/O path is broken", and is what states that privacy mode refuses
     * I/O REVERSIBLY rather than destroying the connection (DDR-1070 sec.5.3). */
    if (nsi(SYS_SET_MODE, MODE_PRIVACY_OFF, 0, 0) != 0)
        fail("phase 4d: SYS_SET_MODE(PRIVACY_OFF) rejected");
    if (nsi(SYS_SOCK_WRITE, h, (long)txbuf, 4) != 4)
        fail("phase 4d: privacy OFF did not restore writes on the same handle");
    (void)nsi(SYS_SOCK_CLOSE, h, 0, 0);

    wr("PRADYOS_PRIVACY_EGRESS_OK\n");

    /* The audit trail has to carry the refusal, not just the return value. */
    struct audit_entry buf[256];
    long n = nsi(SYS_READ_AUDIT, (long)buf, 256, 0);
    if (n <= 0) fail("SYS_READ_AUDIT returned nothing");

    if (!seen(buf, n, id_allowed, AR_PRIVACY_BLOCKED))
        fail("no AR_PRIVACY_BLOCKED record for the allowlisted destination");
    if (!seen(buf, n, id_offlist, AR_PRIVACY_BLOCKED))
        fail("no AR_PRIVACY_BLOCKED record for the off-allowlist destination");

    /* 4e -- the write refusal has to be IN THE TRAIL, keyed on the DESTINATION
     * and not on a handle (DDR-1070 sec.5.2), and under ACTION_NET_EGRESS so a
     * blocked write is not recorded as a blocked connect (DDR-801). */
    if (!seen_t(buf, n, ACTION_NET_EGRESS, id_lo, AR_PRIVACY_BLOCKED))
        fail("no ACTION_NET_EGRESS/AR_PRIVACY_BLOCKED record for the open socket");

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
