/* third_party/lwip-port/pradyos_net.h — kernel-facing NET-B entry points.
 * Plain prototypes only (no lwIP headers) so kmain/idt can call them without
 * pulling the lwIP include surface into the general kernel build. */
#ifndef PRADYOS_NET_H
#define PRADYOS_NET_H
void net_init(void);       /* bring up lwIP over virtio-net (after virtio_net_init) */
void net_poll_tick(void);  /* drive lwIP timers; call from the PIT tick (~every 100 ms) */
#endif
