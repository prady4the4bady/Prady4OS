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
#include "lwip/tcp.h"
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

/* ---- TCP echo server on 0.0.0.0:8007 (smoke-net, ADR-025 §D10) ------------ */
static err_t tcp_echo_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (p == NULL) {                              /* peer closed */
        tcp_close(tpcb);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    kputs("PRADYOS_NET_TCP_OK\r\n");              /* data made it through the real netif */
    tcp_recved(tpcb, p->tot_len);
    for (struct pbuf *q = p; q != NULL; q = q->next)
        tcp_write(tpcb, q->payload, q->len, TCP_WRITE_FLAG_COPY);   /* echo */
    tcp_output(tpcb);
    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_echo_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || newpcb == NULL)
        return ERR_VAL;
    kputs("PRADYOS_NET_TCP_READY\r\n");
    tcp_recv(newpcb, tcp_echo_recv);
    return ERR_OK;
}

static void tcp_echo_init(void) {
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) return;
    if (tcp_bind(pcb, IP_ANY_TYPE, 8007) != ERR_OK) { tcp_close(pcb); return; }
    struct tcp_pcb *lpcb = tcp_listen(pcb);       /* listen() frees the old pcb on success */
    if (!lpcb) { tcp_close(pcb); return; }
    tcp_accept(lpcb, tcp_echo_accept);
}

/* ---- fuzz / hardening self-test (smoke-net-fuzz, ADR-025 §D6/§D10) --------
 * SLIRP can't inject raw L2 frames, so we exercise the receive path in-kernel:
 * synthesise malformed/truncated frames and a bounded SYN flood and feed them
 * straight into the netif input. The pass criterion is survival — no panic, no
 * unbounded allocation — proving length-before-field validation (ADR-025 §D6)
 * and the bounded TCP PCB pool hold under hostile input. Runs once with IRQs
 * masked (same context as RX), so lwIP is never re-entered concurrently. */
static uint16_t csum16(const uint8_t *d, int len, uint32_t seed) {
    uint32_t s = seed;
    for (int i = 0; i + 1 < len; i += 2) s += (uint32_t)(d[i] << 8) | d[i + 1];
    if (len & 1) s += (uint32_t)d[len - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

/* Build an Ethernet+IPv4+TCP SYN to 10.0.2.15:8007 from a varying source. */
static int build_syn(uint8_t *f, uint16_t sport, uint8_t srclast) {
    uint8_t mac[6];
    virtio_net_mac(mac);
    /* Ethernet */
    memcpy(f, mac, 6);                       /* dst = us (unicast) */
    f[6] = 0x52; f[7] = 0x55; f[8] = 0x0a; f[9] = 0x00; f[10] = 0x02; f[11] = 0x02;
    f[12] = 0x08; f[13] = 0x00;              /* IPv4 */
    uint8_t *ip = f + 14;
    ip[0] = 0x45; ip[1] = 0x00; ip[2] = 0x00; ip[3] = 40;   /* ver/ihl, tos, total len */
    ip[4] = sport; ip[5] = 0x13; ip[6] = 0x00; ip[7] = 0x00;/* id, flags/frag */
    ip[8] = 64; ip[9] = 6; ip[10] = 0; ip[11] = 0;          /* ttl, proto=TCP, csum */
    ip[12] = 10; ip[13] = 0; ip[14] = 2; ip[15] = srclast;  /* src 10.0.2.x */
    ip[16] = 10; ip[17] = 0; ip[18] = 2; ip[19] = 15;       /* dst 10.0.2.15 */
    uint16_t ic = csum16(ip, 20, 0); ip[10] = ic >> 8; ip[11] = ic & 0xff;
    uint8_t *tcp = ip + 20;
    tcp[0] = sport >> 8; tcp[1] = sport & 0xff;             /* sport */
    tcp[2] = 0x27; tcp[3] = 0x0f;                           /* dport 9999 (closed: RST, no PCB) */
    tcp[4] = sport; tcp[5] = 0x42; tcp[6] = 0x00; tcp[7] = 0x01;  /* seq */
    tcp[8] = tcp[9] = tcp[10] = tcp[11] = 0;                /* ack */
    tcp[12] = 0x50; tcp[13] = 0x02;                         /* dataoff=5, SYN */
    tcp[14] = 0xfa; tcp[15] = 0xf0;                         /* window */
    tcp[16] = tcp[17] = 0; tcp[18] = tcp[19] = 0;           /* csum, urg */
    /* TCP checksum over pseudo-header (src,dst,proto,len) + segment. */
    uint32_t pseudo = (10u << 8 | 0) + (2u << 8 | srclast)
                    + (10u << 8 | 0) + (2u << 8 | 15)
                    + 6u + 20u;
    uint16_t tc = csum16(tcp, 20, pseudo); tcp[16] = tc >> 8; tcp[17] = tc & 0xff;
    return 54;
}

static void net_fuzz_test(void) {
    static uint8_t f[1600];
    /* 1) Malformed/truncated frames of varying length and content: lengths span
     *    the RX bounds (the <14 and >1514 guards) and the IP/TCP header edges. */
    for (int i = 0; i < 512; i++) {
        unsigned r = pradyos_lwip_rand();
        uint32_t len = r % 1600;                 /* incl. degenerate 0..13 and >1514 */
        for (uint32_t j = 0; j < len && j < sizeof f; j++)
            f[j] = (uint8_t)(pradyos_lwip_rand() >> ((j & 3) * 8));
        if (len >= 14) { f[12] = 0x08; f[13] = 0x00; }   /* bias some toward IPv4 */
        pradyos_netif_rx(f, len);
    }
    /* 2) SYN flood from distinct src ports/IPs to a CLOSED port: every segment
     *    is fully parsed + checksum-verified, then answered with RST and freed —
     *    no PCB is allocated, so memory stays bounded under flood (and no state
     *    lingers to disturb the :8007 echo gate). Survival is the pass criterion. */
    for (int i = 0; i < 256; i++) {
        int n = build_syn(f, (uint16_t)(0x8000 + i), (uint8_t)(100 + (i & 0x3f)));
        pradyos_netif_rx(f, (uint32_t)n);
        if ((i & 0x1f) == 0) sys_check_timeouts();       /* drive the RST/timer path */
    }
    sys_check_timeouts();
    kputs("PRADYOS_NET_FUZZ_OK\r\n");
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
    tcp_echo_init();                             /* TCP echo on :8007 (smoke-net) */
    g_net_ready = 1;
    kputs("[net] lwIP up 10.0.2.15/24\r\n");
    net_loopback_test();
    net_fuzz_test();                             /* malformed-frame + SYN-flood hardening */
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
