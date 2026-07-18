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
static uint32_t g_sock_owner[SOCK_SLOTS];

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

static int sock_denied(int slot) {
    if (slot < 0 || slot >= SOCK_SLOTS)
        return 0;                           /* out of range: let psock_* -EBADF it */
    return g_sock_owner[slot] != current_thread->pid && !current_thread->is_sovereign;
}

static long sys_sock_connect(long a1, long a2, long a3, long a4) {
    (void)a3; (void)a4;
    /* DDR-731: network authority. Agents are granted CAP_NET at spawn; the
     * sovereign operator passes for diagnostics. Everyone else: audited -EPERM
     * (the AETHER pattern — denial is logged, the caller survives). */
    if (!current_thread->is_net && !current_thread->is_sovereign) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    /* DDR-734: CAP_NET is scoped by the egress allowlist (deny-by-default);
     * the sovereign operator bypasses it. Audited like every denial. */
    if (!current_thread->is_sovereign &&
        netallow_check((uint32_t)a1, (uint16_t)a2) != 0) {
        aether_audit(current_thread->pid, 0, 0, AR_CAP_DENIED);
        return -EPERM;
    }
    int slot = psock_connect((uint32_t)a1, (uint16_t)a2);
    if (slot >= 0 && slot < SOCK_SLOTS)
        g_sock_owner[slot] = current_thread->pid;
    return (slot < 0) ? -EMFILE : (long)slot;     /* no free socket / net down */
}

static long sys_sock_write(long a1, long a2, long a3, long a4) {
    (void)a4;
    if (sock_denied((int)a1))
        return -EPERM;                        /* DDR-731: not this caller's slot */
    int len = (int)a3;
    if (len <= 0) return 0;
    if (len > SOCK_IO_MAX) len = SOCK_IO_MAX;
    uint8_t kbuf[SOCK_IO_MAX];
    if (copyin(kbuf, (const void __user *)a2, (size_t)len) < 0)
        return -EFAULT;
    int n = psock_write((int)a1, kbuf, len);
    return (n < 0) ? -EIO : (long)n;
}

static long sys_sock_read(long a1, long a2, long a3, long a4) {
    int slot = (int)a1;
    if (sock_denied(slot))
        return -EPERM;                        /* DDR-731: not this caller's slot */
    int len = (int)a3;
    unsigned timeout_ms = (unsigned)a4;
    if (len <= 0) return 0;
    if (len > SOCK_IO_MAX) len = SOCK_IO_MAX;
    uint8_t kbuf[SOCK_IO_MAX];

    uint64_t deadline = g_ticks + (timeout_ms + 9u) / 10u;   /* PIT @100 Hz */
    for (;;) {
        int n = psock_read(slot, kbuf, len);
        if (n < 0)
            return -EBADF;
        if (n > 0) {
            if (copyout((void __user *)a2, kbuf, (size_t)n) < 0)
                return -EFAULT;
            return (long)n;
        }
        int st = psock_state(slot);
        if (st < 0)
            return -EBADF;
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

static long sys_sock_close(long a1, long a2, long a3, long a4) {
    (void)a2; (void)a3; (void)a4;
    int slot = (int)a1;
    if (sock_denied(slot))
        return -EPERM;                        /* DDR-731: not this caller's slot */
    if (psock_close(slot) < 0)
        return -EBADF;
    if (slot >= 0 && slot < SOCK_SLOTS)
        g_sock_owner[slot] = 0;
    return 0;
}

/* DDR-731 — exit reap (the DDR-729 one-owner pattern): close every proxy slot
 * the exiting pid owns, so a process that dies mid-connection never leaks one
 * of the 8 slots. Called from sched_exit. */
void socket_reap_pid(uint32_t pid) {
    for (int s = 0; s < SOCK_SLOTS; s++)
        if (g_sock_owner[s] == pid) {
            psock_close(s);
            g_sock_owner[s] = 0;
        }
}

/* DDR-734: sovereign-only, append an egress rule. -EPERM (audited) otherwise;
 * -ENOSPC when the bounded list is full. Install-only by design. */
static long sys_net_allow(long a1, long a2, long a3, long a4) {
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
