/* kernel/syscall/sys_socket.c — ring-3 proxy-socket NSI (ADR-027).
 *
 * Four thin handlers over the kernel-owned proxy sockets (third_party/lwip-port).
 * The kernel holds the lwIP PCB; ring 3 sees only a slot index. Every buffer
 * crosses copyin/copyout; a bad pointer is -EFAULT, never a panic. Syscalls run
 * IF=0, so the lwIP calls these make are atomic w.r.t. the RX/PIT IRQ that also
 * drives the stack (ADR-027 D3).
 */
#include "syscall.h"
#include "uaccess.h"
#include "errno.h"
#include "irq.h"          /* g_ticks */
#include "pradyos_net.h"  /* psock_* */

/* psock_state() codes (mirror the enum in lwip_port.c). */
#define PS_CONNECTING 1
#define PS_OPEN       2
#define PS_CLOSING    3
#define PS_ERR        4

#define SOCK_IO_MAX 2048      /* per-call buffer bound (kernel staging) */

static long sys_sock_connect(long a1, long a2, long a3, long a4) {
    (void)a3; (void)a4;
    int slot = psock_connect((uint32_t)a1, (uint16_t)a2);
    return (slot < 0) ? -EMFILE : (long)slot;     /* no free socket / net down */
}

static long sys_sock_write(long a1, long a2, long a3, long a4) {
    (void)a4;
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
    return (psock_close((int)a1) < 0) ? -EBADF : 0;
}

void sys_socket_register(void) {
    syscall_register(SYS_SOCK_CONNECT, sys_sock_connect);
    syscall_register(SYS_SOCK_WRITE,   sys_sock_write);
    syscall_register(SYS_SOCK_READ,    sys_sock_read);
    syscall_register(SYS_SOCK_CLOSE,   sys_sock_close);
}
