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
int psock_connect(uint32_t host_be, uint16_t port);    /* -> slot(0..7) | -1 */
int psock_state(int slot);                              /* PS_* (0=CLOSED..4=ERR) | -1 */
int psock_read(int slot, uint8_t *kbuf, int len);       /* bytes drained (0 if empty) | -1 */
int psock_write(int slot, const uint8_t *kbuf, int len);/* bytes written | -1 */
int psock_close(int slot);                              /* 0 | -1 */
#endif
