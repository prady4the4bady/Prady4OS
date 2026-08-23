/* third_party/lwip-port/pradyos_net.h — kernel-facing NET-B entry points.
 * Plain prototypes only (no lwIP headers) so kmain/idt can call them without
 * pulling the lwIP include surface into the general kernel build. */
#ifndef PRADYOS_NET_H
#define PRADYOS_NET_H
#include <stdint.h>
void net_init(void);       /* bring up lwIP over virtio-net (after virtio_net_init) */
void net_poll_tick(void);  /* drive lwIP timers; call from the PIT tick (~every 100 ms) */

/* Ring-3 proxy sockets (ADR-027). The kernel owns the lwIP PCB; ring 3 holds the
 * returned slot index. Plain ints/buffers only (no lwIP types) so the syscall
 * layer can call these without the lwIP include surface. */
/* DDR-987 sec.10: every proxy-socket call carries the caller's identity, because
 * authority must be checked under the same lock as the operation. The returned
 * value is an opaque HANDLE ((gen << 3) | slot), not a bare slot index.
 * Errors: -1 stale/bad handle (-EBADF), -2 not this caller's slot (-EPERM). */
int psock_connect(uint32_t host_be, uint16_t port, uint32_t owner);
int psock_state(int h, uint32_t owner, int sovereign);
int psock_read(int h, uint32_t owner, int sovereign, uint8_t *kbuf, int len);
int psock_write(int h, uint32_t owner, int sovereign, const uint8_t *kbuf, int len);
int psock_close(int h, uint32_t owner, int sovereign);
void psock_reap_owner(uint32_t pid);
#endif
