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
#include "pmm.h"          /* proxy-socket RX rings come from the PMM pool */
#include "spinlock.h"     /* DDR-987: the lwIP core lock */

/* DDR-987. lwIP is built NO_SYS=1 / SYS_LIGHTWEIGHT_PROT=0, i.e. with NO internal
 * locking, on the assumption recorded in lwipopts.h that this is a single core.
 * That assumption died with ADR-029..031: socket syscalls run on ANY cpu and
 * net_poll_tick() runs from the TIMER ISR (idt.c), where tcp_tmr / tcp_input --
 * and tcp_abort from psock_close -- all FREE segs and pcbs. The local `cli` this
 * file used guards only the current cpu, so a syscall on cpu A walked
 * pcb->unsent inside tcp_output while cpu B freed it: a use-after-free that
 * surfaced as a ring-0 #GP with RAX = 0xDDDDDDDDDDDDDDDD (kheap POISON_FREE)
 * at tcp_output, reached from sys_sock_connect.
 *
 * A SPINLOCK, not a sleeping mutex: one holder is an ISR, where sleeping is
 * illegal. Safe because no region below yields, and every one already ran with
 * interrupts off. Lock order is net -> heap (lwIP allocates via kmalloc, which
 * takes g_heap_lock); kheap never calls the net stack, so there is no cycle. */
static spinlock_t g_net_lock = SPINLOCK_INIT;
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
/* DDR-987 sec.2 (found in review of the first cut). virtio-net's completion
 * handler is registered on MSI-X vector 54 and calls this callback straight from
 * the ISR -- pbuf_alloc / netif.input -> ethernet_input -> ip_input -> tcp_input
 * is lwIP CORE. The first cut of DDR-987 locked the timer ISR and missed this
 * one, leaving the very race it set out to close: an RX interrupt on cpu B
 * entering tcp_input while cpu A held g_net_lock in tcp_output.
 *
 * The lock goes in a WRAPPER, not in pradyos_netif_rx itself, because
 * net_fuzz_test() calls the raw function from inside net_init()'s locked region
 * and g_net_lock is not recursive -- wrapping the callee would self-deadlock at
 * boot. ISR callers get the wrapper; already-locked callers keep the raw one. */
static void pradyos_netif_rx(const uint8_t *frame, uint32_t len);

/* DDR-987 sec.8: one pump iteration under the lock, RELEASED between iterations.
 * net_init() used to hold g_net_lock across net_loopback_tcp_test()'s 200-round
 * loop and net_fuzz_test()'s 256-round loop. Under TCG that is hundreds of ms,
 * and every OTHER cpu's timer ISR spins on the lock with interrupts off for the
 * whole time -- so its per-cpu tick counter freezes and virtio-blk's tick-bounded
 * completion wait expires. That is a regression this lock introduced, not a
 * pre-existing one: the old `cli` never blocked another cpu. */
static void net_pump_locked(void) {
    uint64_t fl = spin_lock_irqsave(&g_net_lock);
    netif_poll_all();
    sys_check_timeouts();
    spin_unlock_irqrestore(&g_net_lock, fl);
}

static void net_timeouts_locked(void) {
    uint64_t fl = spin_lock_irqsave(&g_net_lock);
    sys_check_timeouts();
    spin_unlock_irqrestore(&g_net_lock, fl);
}

static void pradyos_netif_rx_isr(const uint8_t *frame, uint32_t len) {
    uint64_t fl = spin_lock_irqsave(&g_net_lock);
    pradyos_netif_rx(frame, len);
    spin_unlock_irqrestore(&g_net_lock, fl);
}

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
    /* DDR-987 sec.8: net_init() no longer holds the lock across this test, so
     * each lwIP burst takes it here. Setup and send are bounded; the pump is
     * separate so the lock is never held across a poll loop. */
    uint64_t fl = spin_lock_irqsave(&g_net_lock);
    struct udp_pcb *rx = udp_new();
    if (!rx) { spin_unlock_irqrestore(&g_net_lock, fl); return; }
    udp_bind(rx, IP_ADDR_ANY, 7);
    udp_recv(rx, lo_recv, NULL);

    struct udp_pcb *tx = udp_new();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 4, PBUF_RAM);
    int sent = 0;
    if (tx && p) {
        memcpy(p->payload, "ping", 4);
        ip_addr_t lo;
        IP4_ADDR(&lo, 127, 0, 0, 1);
        udp_sendto(tx, p, &lo, 7);
        sent = 1;
    }
    spin_unlock_irqrestore(&g_net_lock, fl);
    if (sent)
        net_pump_locked();                       /* DDR-987 sec.8: deliver the queued loopback pbuf */
    fl = spin_lock_irqsave(&g_net_lock);
    if (p) pbuf_free(p);
    if (tx) udp_remove(tx);
    spin_unlock_irqrestore(&g_net_lock, fl);
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

/* ---- loopback TCP echo client gate (smoke-net-tcp-lo, DDR-753) ------------
 * Drives the TCP CLIENT path end-to-end over 127.0.0.1: connect (3-way
 * handshake) -> write "ping" -> the :8007 echo server echoes -> verify. All
 * delivery is synchronous via netif_poll_all (loopback), so a bounded pump loop
 * converges without timers; the cap guarantees no hang. */
static volatile int g_tcp_lo_echoed;

static err_t tcp_lo_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (p == NULL) { tcp_close(pcb); return ERR_OK; }
    if (err == ERR_OK) {
        char b[8];
        u16_t n = pbuf_copy_partial(p, b, sizeof b, 0);
        if (n >= 4 && b[0] == 'p' && b[1] == 'i' && b[2] == 'n' && b[3] == 'g')
            g_tcp_lo_echoed = 1;
        tcp_recved(pcb, p->tot_len);
    }
    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_lo_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) return err;
    tcp_write(pcb, "ping", 4, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

static void net_loopback_tcp_test(void) {
    /* DDR-987 sec.8: bounded setup under the lock; the 200-round pump below runs
     * OUTSIDE it, one locked burst per iteration. Holding across the whole loop
     * froze every other cpu's timer ISR on this lock. */
    uint64_t fl = spin_lock_irqsave(&g_net_lock);
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { spin_unlock_irqrestore(&g_net_lock, fl); return; }
    tcp_recv(pcb, tcp_lo_recv);
    ip_addr_t lo;
    IP4_ADDR(&lo, 127, 0, 0, 1);
    if (tcp_connect(pcb, &lo, 8007, tcp_lo_connected) != ERR_OK) {
        tcp_close(pcb);
        spin_unlock_irqrestore(&g_net_lock, fl);
        return;
    }
    spin_unlock_irqrestore(&g_net_lock, fl);
    for (int i = 0; i < 200 && !g_tcp_lo_echoed; i++) {
        net_pump_locked();                       /* DDR-987 sec.8: short burst, not a long hold */
    }
    kputs(g_tcp_lo_echoed ? "PRADYOS_NET_TCP_LO_OK\r\n"
                          : "PRADYOS_NET_TCP_LO_FAIL\r\n");
    /* pcb stays resident (one bounded PCB), like the UDP rx pcb. */
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
        pradyos_netif_rx_isr(f, len);            /* DDR-987 sec.8 */
    }
    /* 2) SYN flood from distinct src ports/IPs to a CLOSED port: every segment
     *    is fully parsed + checksum-verified, then answered with RST and freed —
     *    no PCB is allocated, so memory stays bounded under flood (and no state
     *    lingers to disturb the :8007 echo gate). Survival is the pass criterion. */
    for (int i = 0; i < 256; i++) {
        int n = build_syn(f, (uint16_t)(0x8000 + i), (uint8_t)(100 + (i & 0x3f)));
        pradyos_netif_rx_isr(f, (uint32_t)n);     /* DDR-987 sec.8 */
        if ((i & 0x1f) == 0) net_timeouts_locked();      /* DDR-987 sec.8 */
    }
    net_timeouts_locked();                       /* DDR-987 sec.8 */
    kputs("PRADYOS_NET_FUZZ_OK\r\n");
}

static volatile int g_net_ready;   /* set once lwIP is initialised (PIT may fire earlier) */

/* ---- proxy sockets for ring-3 (ADR-027) ----------------------------------
 * The kernel owns the TCP connection; ring 3 holds only a slot index. lwIP is
 * touched here from the SYS_SOCK_* handlers (which run IF=0, so an RX/PIT IRQ
 * cannot reenter the stack mid-call) and from the recv callback (IRQ context,
 * which only buffers into the ring). Rings are PMM-pool pages, not BSS. */
#define PSOCK_N     8
#define PSOCK_RING  4096u          /* power of two */
#define PSOCK_MASK  (PSOCK_RING - 1u)
enum { PS_CLOSED = 0, PS_CONNECTING, PS_OPEN, PS_CLOSING, PS_ERR };

struct proxy_sock {
    struct tcp_pcb *pcb;
    uint8_t *rx;                   /* PSOCK_RING-byte ring from the PMM pool */
    uint16_t head, tail;           /* head: producer (IRQ); tail: consumer (syscall) */
    volatile uint8_t state;
    uint8_t used;
};
static struct proxy_sock g_ps[PSOCK_N];

static uint16_t ps_avail(struct proxy_sock *s) { return (uint16_t)((s->head - s->tail) & PSOCK_MASK); }
static uint16_t ps_space(struct proxy_sock *s) { return (uint16_t)(PSOCK_MASK - ps_avail(s)); }

/* recv (IRQ context): buffer the whole pbuf or refuse it for later redelivery so
 * no byte is ever dropped (flow control via tcp_recved on accepted bytes). */
static err_t ps_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    struct proxy_sock *s = (struct proxy_sock *)arg;
    if (!s) { if (p) pbuf_free(p); return ERR_OK; }
    if (p == NULL) { s->state = PS_CLOSING; return ERR_OK; }   /* peer closed */
    if (err != ERR_OK) { pbuf_free(p); return err; }
    if (ps_space(s) < p->tot_len)
        return ERR_MEM;                          /* ring full: keep pbuf, retry later */
    for (struct pbuf *q = p; q; q = q->next) {
        const uint8_t *d = (const uint8_t *)q->payload;
        for (u16_t i = 0; i < q->len; i++) {
            s->rx[s->head] = d[i];
            s->head = (uint16_t)((s->head + 1) & PSOCK_MASK);
        }
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void ps_err(void *arg, err_t err) {
    (void)err;
    struct proxy_sock *s = (struct proxy_sock *)arg;
    if (s) { s->pcb = NULL; s->state = PS_ERR; }  /* lwIP already freed the pcb */
}

static err_t ps_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)pcb;
    struct proxy_sock *s = (struct proxy_sock *)arg;
    if (s) s->state = (err == ERR_OK) ? PS_OPEN : PS_ERR;
    return ERR_OK;
}

/* Open a connection to host (big-endian a.b.c.d packed) : port. Returns slot. */
int psock_connect(uint32_t host, uint16_t port) {
    if (!g_net_ready) return -1;

    /* DDR-987 sec.2: the scan-and-claim MUST be inside the lock. Unlocked, two
     * cpus could see the same !used slot, both set used=1, both tcp_new(), and
     * both return the same slot number -- one pcb leaked and two owners for one
     * proxy_sock. Page allocation stays outside so the hold covers only the
     * claim; the RX ring is attached under the lock below. Lock order is
     * net -> pmm, matching the existing net -> heap (kmalloc reaches pmm). */
    uint8_t *rxpage = (uint8_t *)(uintptr_t)pmm_alloc_page();
    if (!rxpage) return -1;

    uint64_t fl;
    fl = spin_lock_irqsave(&g_net_lock);            /* DDR-987 */
    int slot = -1;
    for (int i = 0; i < PSOCK_N; i++) if (!g_ps[i].used) { slot = i; break; }
    if (slot < 0) {
        spin_unlock_irqrestore(&g_net_lock, fl);
        pmm_free_page((uint64_t)(uintptr_t)rxpage);
        return -1;
    }
    struct proxy_sock *s = &g_ps[slot];
    s->rx = rxpage;
    s->head = s->tail = 0;
    s->used = 1;
    s->state = PS_CONNECTING;
    s->pcb = tcp_new();
    if (!s->pcb) {
        s->rx = 0; s->used = 0; s->state = PS_CLOSED;   /* release the slot under the lock */
        spin_unlock_irqrestore(&g_net_lock, fl);        /* DDR-987 */
        pmm_free_page((uint64_t)(uintptr_t)rxpage);     /* free the page AFTER release */
        return -1;
    }
    tcp_arg(s->pcb, s);
    tcp_recv(s->pcb, ps_recv);
    tcp_err(s->pcb, ps_err);
    ip_addr_t ip;
    IP4_ADDR(&ip, (host >> 24) & 0xff, (host >> 16) & 0xff, (host >> 8) & 0xff, host & 0xff);
    err_t e = tcp_connect(s->pcb, &ip, port, ps_connected);
    spin_unlock_irqrestore(&g_net_lock, fl);        /* DDR-987 */
    if (e != ERR_OK) { s->state = PS_ERR; return -1; }
    return slot;
}

int psock_state(int slot) {
    if (slot < 0 || slot >= PSOCK_N || !g_ps[slot].used) return -1;
    return g_ps[slot].state;
}

/* Drain up to len bytes into kbuf. Returns bytes copied (0 if the ring is empty);
 * the caller distinguishes EOF/timeout via psock_state(). */
int psock_read(int slot, uint8_t *kbuf, int len) {
    if (slot < 0 || slot >= PSOCK_N) return -1;
    struct proxy_sock *s = &g_ps[slot];
    uint64_t fl;
    fl = spin_lock_irqsave(&g_net_lock);            /* DDR-987 */
    /* DDR-987 sec.2: revalidate INSIDE the lock. The pre-lock check was a TOCTOU
     * window -- psock_close on another cpu could retire the slot and free s->rx
     * between the check and the acquire, leaving this loop reading a freed page. */
    if (!s->used || !s->rx) {
        spin_unlock_irqrestore(&g_net_lock, fl);
        return -1;
    }
    int n = 0;
    while (n < len && s->tail != s->head) {
        kbuf[n++] = s->rx[s->tail];
        s->tail = (uint16_t)((s->tail + 1) & PSOCK_MASK);
    }
    spin_unlock_irqrestore(&g_net_lock, fl);        /* DDR-987 */
    return n;
}

int psock_write(int slot, const uint8_t *kbuf, int len) {
    if (slot < 0 || slot >= PSOCK_N) return -1;
    struct proxy_sock *s = &g_ps[slot];
    uint64_t fl;
    fl = spin_lock_irqsave(&g_net_lock);            /* DDR-987 */
    /* DDR-987 sec.2: revalidate INSIDE the lock -- the pre-lock used/state/pcb
     * check could pass and psock_close then detach the pcb before the acquire,
     * so tcp_sndbuf() would run on a freed pcb. */
    if (!s->used || s->state != PS_OPEN || !s->pcb) {
        spin_unlock_irqrestore(&g_net_lock, fl);
        return -1;
    }
    u16_t snd = tcp_sndbuf(s->pcb);
    if (len > (int)snd) len = (int)snd;
    err_t e = ERR_OK;
    if (len > 0) {
        e = tcp_write(s->pcb, kbuf, (u16_t)len, TCP_WRITE_FLAG_COPY);
        if (e == ERR_OK) tcp_output(s->pcb);
    }
    spin_unlock_irqrestore(&g_net_lock, fl);        /* DDR-987 */
    return (e == ERR_OK) ? len : -1;
}

int psock_close(int slot) {
    if (slot < 0 || slot >= PSOCK_N || !g_ps[slot].used) return -1;
    struct proxy_sock *s = &g_ps[slot];
    uint64_t fl;
    fl = spin_lock_irqsave(&g_net_lock);            /* DDR-987 */
    if (s->pcb) {
        tcp_arg(s->pcb, NULL);
        tcp_recv(s->pcb, NULL);
        tcp_err(s->pcb, NULL);
        if (tcp_close(s->pcb) != ERR_OK)
            tcp_abort(s->pcb);                   /* force teardown on a busy pcb */
        s->pcb = NULL;
    }
    /* DDR-987 sec.2: detach the ring and retire the slot UNDER the lock, then
     * free the page after release. Previously the page was freed after unlock
     * while s->used was still 1, so a concurrent psock_read could copy out of a
     * page already returned to the PMM. */
    uint8_t *rxpage = s->rx;
    s->rx = 0; s->used = 0; s->state = PS_CLOSED;
    spin_unlock_irqrestore(&g_net_lock, fl);        /* DDR-987 */
    if (rxpage) pmm_free_page((uint64_t)(uintptr_t)rxpage);
    return 0;
}

/* ---- entry points (declared in pradyos_net.h) ---------------------------- */

void net_init(void) {
    if (!virtio_net_up())
        return;                                  /* no NIC: skip the stack */
    /* Mask interrupts across init: after virtio_net_set_rx / g_net_ready, an RX or
     * PIT IRQ would otherwise re-enter lwIP (NO_SYS, not reentrant) mid-setup. */
    uint64_t fl;
    fl = spin_lock_irqsave(&g_net_lock);            /* DDR-987 */
    lwip_init();                                 /* also creates the 127.0.0.1 loopif */
    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip, 10, 0, 2, 15);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);
    netif_add(&g_netif, &ip, &mask, &gw, NULL, pradyos_netif_init, netif_input);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    virtio_net_set_rx(pradyos_netif_rx_isr);     /* DDR-987 sec.2: RX IRQ -> lwIP, LOCKED */
    tcp_echo_init();                             /* TCP echo on :8007 (smoke-net) */
    g_net_ready = 1;
    kputs("[net] lwIP up 10.0.2.15/24\r\n");
    /* DDR-987 sec.8: release BEFORE the self-tests. Holding across their pump
     * loops stalls every other cpu's timer ISR on this lock (see net_pump_locked).
     * The tests take the lock themselves, one iteration at a time. */
    spin_unlock_irqrestore(&g_net_lock, fl);        /* DDR-987 */
    net_loopback_test();
    net_loopback_tcp_test();                     /* DDR-753: TCP client echo over loopback */
    net_fuzz_test();                             /* malformed-frame + SYN-flood hardening */
}

/* Called from the PIT tick (every ~100 ms): drive lwIP's timers and flush any
 * queued loopback traffic. No-op until net_init has run. */
void net_poll_tick(void) {
    if (!g_net_ready)
        return;
    /* DDR-987: this runs in the TIMER ISR and is the side that FREES --
     * sys_check_timeouts -> tcp_tmr, netif_poll_all -> tcp_input. It had no
     * cross-cpu guard at all, so it could free a seg out from under a syscall
     * running tcp_output on another cpu. */
    uint64_t fl = spin_lock_irqsave(&g_net_lock);
    sys_check_timeouts();
    netif_poll_all();
    spin_unlock_irqrestore(&g_net_lock, fl);
}
