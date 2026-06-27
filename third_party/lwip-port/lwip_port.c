/* third_party/lwip-port/lwip_port.c — PRADYOS lwIP port (NET-B, ADR-025).
 *
 * First-party (-Werror): the kernel-side glue lwIP needs (allocator, rand, diag,
 * timer) plus the virtio-net <-> lwIP netif bridge and net_init/net_poll_tick.
 * Compiled by the kernel build with the lwIP include path; the lwIP core itself
 * is in build/lwip/liblwip.a. */
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/udp.h"
#include "lwip/timeouts.h"
#include "lwip/ip_addr.h"
#include "netif/ethernet.h"

#include "kheap.h"
#include "console.h"
#include "irq.h"          /* g_ticks */
#include "string.h"
#include "virtio_net.h"
#include "pradyos_net.h"

/* ---- lwIP platform shims (declared in lwipopts.h / arch/cc.h) -------------- */
void *pradyos_lwip_malloc(unsigned long size)            { return kmalloc((size_t)size); }
void  pradyos_lwip_free(void *p)                         { kfree(p); }
void *pradyos_lwip_calloc(unsigned long n, unsigned long size) {
    size_t total = (size_t)n * (size_t)size;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}
/* A few lwIP paths call the libc names directly (not via mem_clib_*); route them
 * to the kernel allocator too (matches the third_party/lwip-port/stdlib.h shim). */
void *malloc(size_t size)            { return kmalloc(size); }
void  free(void *p)                  { kfree(p); }
void *calloc(size_t n, size_t size)  { return pradyos_lwip_calloc(n, size); }

/* Simple xorshift PRNG seeded from the tick counter — entropy for TCP ISN / SYN
 * cookies. Not cryptographic, but unpredictable enough to harden the seq space
 * against off-path guessing on this single-host test target. */
unsigned int pradyos_lwip_rand(void) {
    static unsigned int s;
    if (!s) s = (unsigned int)(g_ticks ^ 0x9E3779B9u) | 1u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

void pradyos_lwip_diag(const char *fmt, ...) { (void)fmt; }   /* LWIP_DEBUG=0: no-op */
void pradyos_lwip_assert(const char *msg) {
    kputs("[lwip-assert] ");
    kputs(msg ? msg : "(null)");
    kputs("\r\n");
    /* Do NOT halt the kernel on a stack assert — log and continue; the network
     * path is non-critical and must never DoS the system. */
}

/* sys_now(): lwIP timer base in milliseconds (PIT @100 Hz -> 10 ms/tick). */
unsigned int sys_now(void) { return (unsigned int)(g_ticks * 10u); }

int atoi(const char *s) {
    int n = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}

/* ---- virtio-net <-> lwIP netif bridge (ADR-025 §D7) ----------------------- */
static struct netif g_netif;

/* lwIP -> wire: flatten the pbuf chain and hand it to the driver TX. */
static err_t pradyos_linkoutput(struct netif *netif, struct pbuf *p) {
    (void)netif;
    static uint8_t frame[1600];
    if (p->tot_len > sizeof frame)
        return ERR_BUF;
    pbuf_copy_partial(p, frame, p->tot_len, 0);
    return virtio_net_tx(frame, p->tot_len) == 0 ? ERR_OK : ERR_IF;
}

/* netif init callback: fill in MAC/MTU/flags + output hooks. */
static err_t pradyos_netif_init(struct netif *netif) {
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->mtu = 1500;
    netif->hwaddr_len = 6;
    virtio_net_mac(netif->hwaddr);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    netif->output = etharp_output;
    netif->linkoutput = pradyos_linkoutput;
    return ERR_OK;
}

/* wire -> lwIP: called from the virtio-net IRQ with one Ethernet frame. Copies
 * it into a pooled pbuf and injects it; never reads past `len` (ADR-025 §D6). */
static void pradyos_netif_rx(const uint8_t *frame, uint32_t len) {
    if (len < 14 || len > 1514)
        return;                                  /* not a plausible Ethernet frame */
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (!p)
        return;                                  /* out of pbufs: drop */
    if (pbuf_take(p, frame, (u16_t)len) != ERR_OK) {
        pbuf_free(p);
        return;
    }
    if (g_netif.input(p, &g_netif) != ERR_OK)    /* = netif_input -> ethernet_input */
        pbuf_free(p);
}

/* ---- loopback UDP echo gate (smoke-net-lo, ADR-025 §D10) ------------------ */
static void lo_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                    const ip_addr_t *addr, u16_t port) {
    (void)arg; (void)pcb; (void)addr; (void)port;
    kputs("PRADYOS_NET_LO_OK\r\n");
    pbuf_free(p);
}

static void net_loopback_test(void) {
    struct udp_pcb *rx = udp_new();
    if (!rx) return;
    udp_bind(rx, IP_ADDR_ANY, 7);
    udp_recv(rx, lo_recv, NULL);

    struct udp_pcb *tx = udp_new();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 4, PBUF_RAM);
    if (tx && p) {
        memcpy(p->payload, "ping", 4);
        ip_addr_t lo;
        IP4_ADDR(&lo, 127, 0, 0, 1);
        udp_sendto(tx, p, &lo, 7);
        netif_poll_all();                        /* deliver the queued loopback pbuf */
    }
    if (p) pbuf_free(p);
    if (tx) udp_remove(tx);
    /* rx pcb stays bound so later loopback traffic still echoes. */
}

/* ---- entry points (declared in pradyos_net.h) ---------------------------- */
static volatile int g_net_ready;   /* set once lwIP is initialised (PIT may fire earlier) */

void net_init(void) {
    if (!virtio_net_up())
        return;                                  /* no NIC: skip the stack */
    /* Mask interrupts across init: after virtio_net_set_rx / g_net_ready, an RX or
     * PIT IRQ would otherwise re-enter lwIP (NO_SYS, not reentrant) mid-setup. */
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    lwip_init();                                 /* also creates the 127.0.0.1 loopif */
    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip, 10, 0, 2, 15);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);
    netif_add(&g_netif, &ip, &mask, &gw, NULL, pradyos_netif_init, netif_input);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    virtio_net_set_rx(pradyos_netif_rx);         /* RX IRQ -> lwIP */
    g_net_ready = 1;
    kputs("[net] lwIP up 10.0.2.15/24\r\n");
    net_loopback_test();
    __asm__ volatile("push %0; popfq" :: "r"(fl) : "memory", "cc");
}

/* Called from the PIT tick (every ~100 ms): drive lwIP's timers and flush any
 * queued loopback traffic. No-op until net_init has run. */
void net_poll_tick(void) {
    if (!g_net_ready)
        return;
    sys_check_timeouts();
    netif_poll_all();
}
