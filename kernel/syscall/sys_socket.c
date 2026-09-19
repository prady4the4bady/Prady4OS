/* kernel/syscall/sys_socket.c — ring-3 proxy-socket NSI (ADR-027).
 *
 * Four thin handlers over the kernel-owned proxy sockets (third_party/lwip-port).
 * The kernel holds the lwIP PCB; ring 3 sees only a slot index. Every buffer
 * crosses copyin/copyout; a bad pointer is -EFAULT, never a panic. Syscalls run
 * IF=0, so the lwIP calls these make are atomic w.r.t. the RX/PIT IRQ that also
 * drives the stack (ADR-027 D3).
 */
#include "syscall.h"
#include "sched.h"        /* current_thread: is_net / is_sovereign / pid (DDR-731) */
#include "uaccess.h"
#include "errno.h"
#include "irq.h"          /* g_ticks */
#include "aether.h"       /* aether_audit AR_CAP_DENIED (denials are audited) */
#include "pradyos_net.h"  /* psock_* */

/* psock_state() codes (mirror the enum in lwip_port.c). */
#define PS_CONNECTING 1
#define PS_OPEN       2
#define PS_CLOSING    3
#define PS_ERR        4

#define SOCK_IO_MAX 2048      /* per-call buffer bound (kernel staging) */

/* DDR-731 — per-slot ownership. The proxy slots are a global table of 8; before
 * this, ANY process could read/inject/close ANY slot by index. owner[slot] is
 * the pid that connected it (0 = free), set at connect, cleared at close/reap.
 * Single check point for WRITE/READ/CLOSE: owner or the sovereign operator. */
#define SOCK_SLOTS 8
/* DDR-987 sec.10: g_sock_owner[] and sock_denied() are GONE. They were the
 * time-of-check half of a cross-layer TOCTOU -- the owner was read here without
 * a lock and psock_* ran afterwards, so a close+reuse on another cpu left the
 * operation on a different connection. Ownership now lives on the slot in
 * lwip_port.c and is validated under g_net_lock together with the operation. */

/* DDR-734 — per-host egress allowlist for CAP_NET callers. Bounded, append-only
 * (no runtime revocation surface — policy changes are a config edit + reboot),
 * installed by the sovereign daemon (SYS_NET_ALLOW) from /etc/aether/config net= lines
 * BEFORE any agent spawns. EMPTY LIST = DENY-ALL for agents; the sovereign
 * operator bypasses (it is the authority that installs the list). */
#define NET_ALLOW_MAX 8
struct net_allow { uint32_t host_be; uint16_t port; };   /* port 0 = any port */
static struct net_allow g_net_allow[NET_ALLOW_MAX];
static unsigned g_net_allow_n;

/* 0 = allowed, -1 = no matching rule. Exposed (non-static) for the kmain boot
 * self-test, which proves match/deny without a live connection. */
int netallow_check(uint32_t host_be, uint16_t port);
int netallow_check(uint32_t host_be, uint16_t port) {
    for (unsigned i = 0; i < g_net_allow_n; i++)
        if (g_net_allow[i].host_be == host_be &&
            (g_net_allow[i].port == 0 || g_net_allow[i].port == port))
            return 0;
    return -1;
}

/* Install one rule (kmain self-test uses this too; ring 3 goes via the NSI). */
int netallow_add(uint32_t host_be, uint16_t port);
int netallow_add(uint32_t host_be, uint16_t port) {
    if (g_net_allow_n >= NET_ALLOW_MAX)
        return -1;
    g_net_allow[g_net_allow_n].host_be = host_be;
    g_net_allow[g_net_allow_n].port = port;
    g_net_allow_n++;
    return 0;
}

/* Map the port's error shape onto errno. -2 = not this caller's slot (DDR-731
 * per-slot ownership, what user/capnettest.c gates on); -1 = stale/bad handle. */
/* DDR-988 sec.10: three distinct proxy-socket failures, three distinct errnos.
 * -2 PSOCK_DENIED = not this caller's slot; -3 PSOCK_EIO = the send itself
 * failed; anything else = a stale/closed handle. */
static long sock_err(int rc) {
    if (rc == -2) return -EPERM;
    if (rc == -3) return -EIO;
    return -EBADF;
}

static long sys_sock_connect(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a3; (void)a4;
    /* DDR-802: privacy mode refuses egress BEFORE any other question is asked.
     *
     * The ordering is the substance of the control, not a detail:
     *  - before the allowlist, because an allowlisted destination would
     *    otherwise be permitted and then blocked, and the log would show a
     *    policy decision that did not govern the outcome;
     *  - before the CAP_NET check, so a caller with the capability and one
     *    without are refused identically — otherwise the refusal leaks whether
     *    the caller holds it;
     *  - AHEAD OF THE DDR-800 SOVEREIGN BYPASS. This is the one place DDR-802
     *    overrides DDR-800, deliberately. The bypass exists so an operator can
     *    diagnose the network; privacy mode is that same operator's explicit
     *    instruction that nothing leaves. Honouring the bypass here would let
     *    the flag override the control the operator just set. The refusal is
     *    audited with the sovereign's own pid, so the attempt stays visible. */
    if (aether_privacy_active()) {
        aether_audit(current_thread->pid, ACTION_NET_CONNECT,
                     AETHER_DEST_ID((uint32_t)a1, (uint16_t)a2),
                     AR_PRIVACY_BLOCKED);
        return -EPERM;
    }
    /* DDR-731: network authority. Agents are granted CAP_NET at spawn; the
     * sovereign operator passes for diagnostics. Everyone else: audited -EPERM
     * (the AETHER pattern — denial is logged, the caller survives). */
    if (!current_thread->is_net && !current_thread->is_sovereign) {
        aether_audit(current_thread->pid, ACTION_NET_CONNECT,
                     AETHER_DEST_ID((uint32_t)a1, (uint16_t)a2), AR_CAP_DENIED);
        return -EPERM;
    }
    /* DDR-734: CAP_NET is scoped by the egress allowlist (deny-by-default);
     * the sovereign operator bypasses it. Audited like every denial. */
    if (!current_thread->is_sovereign &&
        netallow_check((uint32_t)a1, (uint16_t)a2) != 0) {
        aether_audit(current_thread->pid, ACTION_NET_CONNECT,
                     AETHER_DEST_ID((uint32_t)a1, (uint16_t)a2), AR_CAP_DENIED);
        return -EPERM;
    }

    /* DDR-800 (R1): the sovereign exemption stays — an operator needs egress to
     * diagnose the network, and requiring an allowlist rule first would mean
     * needing network diagnostics to debug the path that installs them. What
     * was missing is the RECORD.
     *
     * Before this, a denied agent produced an audit entry and a sovereign
     * thread reaching any host on the internet produced nothing: the one
     * category of egress with no limits was also the one with no evidence.
     *
     * Emitted only when the flag is what ALLOWED the call — no CAP_NET, or a
     * destination off the allowlist. A sovereign thread that would have passed
     * anyway is recorded below as an ordinary connect, so the code keeps
     * meaning "this happened because of operator authority". */
    if (current_thread->is_sovereign &&
        (!current_thread->is_net ||
         netallow_check((uint32_t)a1, (uint16_t)a2) != 0)) {
        aether_audit(current_thread->pid, ACTION_NET_CONNECT,
                     AETHER_DEST_ID((uint32_t)a1, (uint16_t)a2),
                     AR_SOVEREIGN_BYPASS);
    }
    /* DDR-801 (R3): record the ALLOWED-BY-POLICY case too. Before this, the
     * denial path and the DDR-800 bypass path were both audited and the
     * ordinary success path was not — so the log could answer "what was
     * refused" and "what the operator overrode", but not "what actually went
     * out". netallow_check() returning 0 is a decision; an unrecorded decision
     * is not an audit trail.
     *
     * Emitted AFTER the authority decision and BEFORE psock_connect(): a
     * connect that policy permitted but that then fails on -EMFILE or a dead
     * network is still an authorised egress attempt, and that is what is being
     * audited. Logging only TCP-level successes would make the trail depend on
     * network conditions. */
    if (!(current_thread->is_sovereign &&
          (!current_thread->is_net ||
           netallow_check((uint32_t)a1, (uint16_t)a2) != 0))) {
        aether_audit(current_thread->pid, ACTION_NET_CONNECT,
                     AETHER_DEST_ID((uint32_t)a1, (uint16_t)a2),
                     AR_NET_CONNECT);
    }

    int h = psock_connect((uint32_t)a1, (uint16_t)a2, current_thread->pid);
    return (h < 0) ? -EMFILE : (long)h;           /* no free socket / net down */
}

/* DDR-1070 — privacy mode on the I/O paths.
 *
 * DDR-802 put the privacy check in sys_sock_connect and nowhere else, so the
 * control governed OPENING a channel and not USING one: a proxy socket that was
 * already open when the operator switched privacy mode on kept sending and kept
 * receiving. That is the case the control exists for -- sec.93 above states the
 * design as "the operator's explicit instruction that nothing leaves", and it is
 * ordered ahead of the DDR-800 sovereign bypass precisely so it is
 * unconditional.
 *
 * Checked HERE and not inside psock_write/psock_read under g_net_lock. DDR-987
 * sec.10 pushed the OWNERSHIP check down there because the thing being checked
 * -- the owner of slot N -- could change identity between the check and the
 * operation, so the operation landed on a different connection. Privacy mode is
 * a GLOBAL flag, not a property of a slot; there is no identity to change
 * underneath it, and pushing it down would give the lwIP port an AETHER
 * dependency it does not currently have (grep: zero matches).
 *
 * The refusal is audited with the peer taken off the slot, so the record joins
 * up with the connect record for the same conversation; ACTION_NET_EGRESS keeps
 * "a blocked write" distinct from "a blocked connect" (DDR-801: the record
 * states the decision that was actually made). An unresolvable handle audits
 * dest 0 and still returns -EPERM -- under privacy mode a caller learns nothing
 * about whether the handle was theirs, which is the same reasoning DDR-802
 * records for refusing capability-holders and non-holders identically.
 *
 * NOT applied to sys_sock_close: refusing to close would strand slots and leak
 * the very connections the operator wants stopped. */
static int privacy_refuses_io(long handle) {
    if (!aether_privacy_active())
        return 0;
    uint32_t host_be = 0;
    uint16_t port = 0;
    (void)psock_dest((int)handle, current_thread->pid,
                     current_thread->is_sovereign, &host_be, &port);
    aether_audit(current_thread->pid, ACTION_NET_EGRESS,
                 AETHER_DEST_ID(host_be, port), AR_PRIVACY_BLOCKED);
    return 1;
}

static long sys_sock_write(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a4;
    if (privacy_refuses_io(a1)) return -EPERM;          /* DDR-1070 */
    int len = (int)a3;
    if (len <= 0) return 0;
    if (len > SOCK_IO_MAX) len = SOCK_IO_MAX;
    uint8_t kbuf[SOCK_IO_MAX];
    if (copyin(kbuf, (const void __user *)a2, (size_t)len) < 0)
        return -EFAULT;
    /* DDR-987 sec.10: authority is checked inside psock_write, under the lock. */
    int n = psock_write((int)a1, current_thread->pid,
                        current_thread->is_sovereign, kbuf, len);
    /* DDR-988 sec.10: was `(n < 0) ? -EIO : n`, which flattened a STALE handle
     * into -EIO -- the read and close paths return -EBADF for the same case. */
    return (n < 0) ? sock_err(n) : (long)n;
}

static long sys_sock_read(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    if (privacy_refuses_io(a1)) return -EPERM;          /* DDR-1070 */
    int slot = (int)a1;                       /* opaque handle (DDR-987 sec.10) */
    int len = (int)a3;
    unsigned timeout_ms = (unsigned)a4;
    if (len <= 0) return 0;
    if (len > SOCK_IO_MAX) len = SOCK_IO_MAX;
    uint8_t kbuf[SOCK_IO_MAX];

    uint64_t deadline = g_ticks + (timeout_ms + 9u) / 10u;   /* PIT @100 Hz */
    for (;;) {
        int n = psock_read(slot, current_thread->pid,
                           current_thread->is_sovereign, kbuf, len);
        if (n < 0)
            return sock_err(n);               /* -EPERM vs -EBADF (DDR-731) */
        if (n > 0) {
            if (copyout((void __user *)a2, kbuf, (size_t)n) < 0)
                return -EFAULT;
            return (long)n;
        }
        int st = psock_state(slot, current_thread->pid,
                             current_thread->is_sovereign);
        if (st < 0)
            return sock_err(st);
        if (st == PS_CLOSING)
            return 0;                              /* ring drained + peer closed = EOF */
        if (st == PS_ERR)
            return -EIO;
        if (g_ticks >= deadline)
            return 0;                              /* timeout, no data */
        /* Wait for an IRQ (PIT/RX) so the recv callback can fill the ring even if
         * this agent is the only runnable thread; re-check with IF masked. */
        __asm__ volatile("sti; hlt; cli");
    }
}

static long sys_sock_close(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a2; (void)a3; (void)a4;
    int rc = psock_close((int)a1, current_thread->pid,
                         current_thread->is_sovereign);
    if (rc < 0)
        return sock_err(rc);                  /* -EPERM vs -EBADF (DDR-731) */
    return 0;
}

/* DDR-731 — exit reap (the DDR-729 one-owner pattern): close every proxy slot
 * the exiting pid owns, so a process that dies mid-connection never leaks one
 * of the 8 slots. Called from sched_exit. */
void socket_reap_pid(uint32_t pid) {
    psock_reap_owner(pid);                    /* DDR-987 sec.10: under g_net_lock */
}

/* DDR-734: sovereign-only, append an egress rule. -EPERM (audited) otherwise;
 * -ENOSPC when the bounded list is full. Install-only by design. */
static long sys_net_allow(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a3; (void)a4;
    if (!current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    return (netallow_add((uint32_t)a1, (uint16_t)a2) < 0) ? -ENOSPC : 0;
}

void sys_socket_register(void) {
    syscall_register(SYS_SOCK_CONNECT, sys_sock_connect);
    syscall_register(SYS_SOCK_WRITE,   sys_sock_write);
    syscall_register(SYS_SOCK_READ,    sys_sock_read);
    syscall_register(SYS_SOCK_CLOSE,   sys_sock_close);
    syscall_register(SYS_NET_ALLOW,    sys_net_allow);    /* DDR-734 */
}
