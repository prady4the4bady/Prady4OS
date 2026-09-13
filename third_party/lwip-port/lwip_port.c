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

/* ---- DDR-988: deferred lwIP work, drained by whoever RELEASES g_net_lock ----
 *
 * DDR-987 sec.11 stopped the timer ISR blocking on g_net_lock, but justified the
 * dropped tick with "the holder is inside lwIP, so the timer work is being
 * done". That is false: of this lock's holders only net_pump_locked and
 * net_timeouts_locked call sys_check_timeouts(). psock_read (which drains up to
 * 2048 bytes), psock_write, psock_close and the RX path all hold it without
 * running any timer, so a tick skipped behind one of them was lost outright --
 * with one attempt per event and no retry, that starves TCP retransmit and
 * delayed-ACK for as long as holders keep arriving. sec.11 also LEFT the
 * blocking acquire in the RX ISR, which is the same freeze mechanism on the
 * busier path: net_complete() calls it up to 64 times per IRQ, and its critical
 * section is the whole lwIP receive path, not the bounded pbuf_alloc sec.11
 * claimed.
 *
 * So: neither ISR ever blocks, and deferred work is serviced on the RELEASE
 * path instead of by a future trylock that may never win. Every acquire is
 * followed by a release in bounded time (no holder yields or sleeps, and sec.8
 * removed the two multi-hundred-round loops), so pending work is always drained
 * within one holder's critical section -- or, if the lock was free, by
 * net_poll_tick itself. */
#define NET_RXQ_N        16u
#define NET_RXQ_FRAME    1514u          /* max Ethernet payload lwIP will accept */
#define NET_DRAIN_ROUNDS 2

struct net_rxq_ent { uint16_t len; uint8_t data[NET_RXQ_FRAME]; };

static spinlock_t g_net_rxq_lock = SPINLOCK_INIT;   /* order: net -> rxq -> heap */
static struct net_rxq_ent g_net_rxq[NET_RXQ_N];
static uint32_t g_net_rxq_head, g_net_rxq_tail;
static uint8_t  g_net_rx_stage[NET_RXQ_FRAME];      /* drainer holds g_net_lock */
static uint32_t g_net_timer_pending;

/* DDR-988 sec.5. Counters are read by the [hb] heartbeat in idt.c. Atomic
 * because they are bumped from ISRs on several cpus at once and `cli` serializes
 * only the local one -- the plain ++ sec.11 used could lose increments. */
uint64_t g_net_tick_skipped;    /* timer events deferred instead of polled  */
uint64_t g_net_rx_deferred;     /* frames queued instead of injected inline */
uint64_t g_net_rx_dropped;      /* frames LOST: ring full or rxq contended  */
#define NET_CNT_INC(c) __atomic_add_fetch(&(c), 1, __ATOMIC_RELAXED)

/* ISR side: copy the frame out (virtio re-arms the buffer the moment we return)
 * and leave. Takes only g_net_rxq_lock, and only with trylock, so an RX
 * interrupt can never spin with interrupts off. A frame lost to a full ring or
 * to two cpus in RX at once is a real loss and is counted; TCP retransmits.
 * A blocked ISR, by contrast, freezes a cpu -- that is the trade, made
 * explicitly this time. */
static void net_rxq_push(const uint8_t *frame, uint32_t len) {
    if (len < 14u || len > NET_RXQ_FRAME)
        return;                                  /* not a plausible Ethernet frame */
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    if (spin_trylock(&g_net_rxq_lock)) {
        uint32_t next = (g_net_rxq_head + 1u) % NET_RXQ_N;
        if (next == g_net_rxq_tail) {            /* ring full */
            spin_unlock(&g_net_rxq_lock);
            NET_CNT_INC(g_net_rx_dropped);
        } else {
            g_net_rxq[g_net_rxq_head].len = (uint16_t)len;
            memcpy(g_net_rxq[g_net_rxq_head].data, frame, (size_t)len);
            g_net_rxq_head = next;
            spin_unlock(&g_net_rxq_lock);
            NET_CNT_INC(g_net_rx_deferred);
        }
    } else {
        NET_CNT_INC(g_net_rx_dropped);
    }
    __asm__ volatile("push %0; popfq" :: "r"(fl) : "memory", "cc");
}

/* Drain side: caller holds g_net_lock, so g_net_rx_stage is exclusive. Trylock
 * here too -- the drainer may itself be net_poll_tick in the timer ISR. Losing a
 * round to contention costs nothing: the next release drains unconditionally. */
static int net_rxq_pop(uint16_t *len_out) {
    uint64_t fl;
    int got = 0;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    if (spin_trylock(&g_net_rxq_lock)) {
        if (g_net_rxq_tail != g_net_rxq_head) {
            uint16_t n = g_net_rxq[g_net_rxq_tail].len;
            memcpy(g_net_rx_stage, g_net_rxq[g_net_rxq_tail].data, (size_t)n);
            g_net_rxq_tail = (g_net_rxq_tail + 1u) % NET_RXQ_N;
            *len_out = n;
            got = 1;
        }
        spin_unlock(&g_net_rxq_lock);
    }
    __asm__ volatile("push %0; popfq" :: "r"(fl) : "memory", "cc");
    return got;
}

/* Run whatever was deferred. MUST be called with g_net_lock held: everything it
 * touches is lwIP core. Bounded by NET_RXQ_N frames per round and
 * NET_DRAIN_ROUNDS rounds -- RX work originates at the NIC, not inside lwIP, so
 * a drain cannot feed itself; the round cap is belt-and-braces, not load-bearing. */
static void net_drain_locked(void) {
    for (int round = 0; round < NET_DRAIN_ROUNDS; round++) {
        int did = 0;
        for (uint32_t i = 0; i < NET_RXQ_N; i++) {
            uint16_t n = 0;
            if (!net_rxq_pop(&n))
                break;
            pradyos_netif_rx(g_net_rx_stage, n);
            did = 1;
        }
        /* Exchange, not test-and-clear: a set racing with this drain must not be
         * swallowed -- it either lands before the exchange (serviced now) or
         * after it (serviced by the next release). */
        if (__atomic_exchange_n(&g_net_timer_pending, 0u, __ATOMIC_ACQ_REL)) {
            sys_check_timeouts();
            netif_poll_all();
            did = 1;
        }
        if (!did)
            break;
    }
}

/* The only correct way to release g_net_lock: drain, THEN unlock. */
static void net_unlock(uint64_t fl) {
    net_drain_locked();
    spin_unlock_irqrestore(&g_net_lock, fl);
}

/* DDR-988 sec.9: synchronous injection, for kernel self-tests that are NOT
 * interrupts. net_fuzz_test() used the ISR wrapper; once that wrapper became a
 * 16-slot enqueue, 613 of its 768 frames were dropped on the floor and never
 * reached lwIP -- smoke-net-fuzz still passed while testing almost nothing.
 * A self-test has a thread and can afford to wait, so it takes the lock and
 * calls the raw injector. Per sec.8 the lock is taken PER FRAME, never held
 * across the loop. */
static void net_inject_locked(const uint8_t *frame, uint32_t len) {
    uint64_t fl = spin_lock_irqsave(&g_net_lock);
    pradyos_netif_rx(frame, len);
    net_unlock(fl);
}

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
    net_unlock(fl);
}

static void net_timeouts_locked(void) {
    uint64_t fl = spin_lock_irqsave(&g_net_lock);
    sys_check_timeouts();
    net_unlock(fl);
}

/* DDR-988 sec.3: the RX interrupt no longer enters lwIP at all. It queues the
 * frame; the next cpu to release g_net_lock injects it. */
static void pradyos_netif_rx_isr(const uint8_t *frame, uint32_t len) {
    net_rxq_push(frame, len);
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
    if (!rx) { net_unlock(fl); return; }
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
    net_unlock(fl);
    if (sent)
        net_pump_locked();                       /* DDR-987 sec.8: deliver the queued loopback pbuf */
    fl = spin_lock_irqsave(&g_net_lock);
    if (p) pbuf_free(p);
    if (tx) udp_remove(tx);
    net_unlock(fl);
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
    if (!pcb) { net_unlock(fl); return; }
    tcp_recv(pcb, tcp_lo_recv);
    ip_addr_t lo;
    IP4_ADDR(&lo, 127, 0, 0, 1);
    if (tcp_connect(pcb, &lo, 8007, tcp_lo_connected) != ERR_OK) {
        tcp_close(pcb);
        net_unlock(fl);
        return;
    }
    net_unlock(fl);
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
        net_inject_locked(f, len);               /* DDR-988 sec.9: NOT the ISR path */
    }
    /* 2) SYN flood from distinct src ports/IPs to a CLOSED port: every segment
     *    is fully parsed + checksum-verified, then answered with RST and freed —
     *    no PCB is allocated, so memory stays bounded under flood (and no state
     *    lingers to disturb the :8007 echo gate). Survival is the pass criterion. */
    for (int i = 0; i < 256; i++) {
        int n = build_syn(f, (uint16_t)(0x8000 + i), (uint8_t)(100 + (i & 0x3f)));
        net_inject_locked(f, (uint32_t)n);        /* DDR-988 sec.9 */
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
    /* DDR-987 sec.10. The owner lives HERE, not in sys_socket.c, so that the
     * authority check and the operation happen under one lock. Previously
     * sock_denied() read g_sock_owner[slot] unlocked and the syscall called
     * psock_* afterwards, so another cpu could close and reuse the slot in
     * between and the operation landed on a different connection. `gen` is
     * monotonic and rides in the handle, so even the SAME owner reconnecting
     * cannot be reached through a stale handle. */
    uint32_t owner;                /* pid that claimed the slot; 0 = unowned */
    uint32_t gen;                  /* generation of this claim; 0 = never live */
    /* DDR-1070: the destination this slot was opened to, kept so a refusal on
     * the I/O path can be audited with a real AETHER_DEST_ID rather than with a
     * handle -- a handle is not a destination, and the write refusal has to
     * join up with the connect record for the same conversation (sec.5.2).
     * Stored from psock_connect's own arguments rather than re-derived from
     * pcb->remote_ip, which would round-trip through lwIP's representation of a
     * value the caller handed us verbatim. */
    uint32_t host_be;              /* packed a.b.c.d, as the NSI takes it */
    uint16_t port;
};

/* Handle = (gen << 3) | slot. PSOCK_N is 8, so the slot is exactly 3 bits and a
 * decode can never be out of range. g_psock_gen starts at 1, so a handle of 0 --
 * which user/capnettest.c passes deliberately -- carries gen 0 and matches no
 * live slot. Ring 3 treats the handle as opaque (agent_base.c only tests < 0). */
#define PSOCK_SLOT_BITS 3
#define PSOCK_H(g, sl)  ((int)(((g) << PSOCK_SLOT_BITS) | (uint32_t)(sl)))
#define PSOCK_SLOT(h)   ((int)((uint32_t)(h) & ((1u << PSOCK_SLOT_BITS) - 1u)))
#define PSOCK_GEN(h)    ((uint32_t)(h) >> PSOCK_SLOT_BITS)
static uint32_t g_psock_gen = 1;
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

/* DDR-987 sec.10. Resolve a handle to its slot, with authority, UNDER THE LOCK.
 * Errno shape is load-bearing and matches the pre-existing DDR-731 contract that
 * user/capnettest.c gates on:
 *   - a handle naming a slot this caller does not own -> PSOCK_DENIED (-EPERM),
 *     INCLUDING an unused slot. capnettest passes handle 0 while owning nothing
 *     and requires exactly -EPERM, so "not yours" must outrank "not open".
 *   - a slot the caller DOES own but whose generation no longer matches, or that
 *     is closed -> PSOCK_STALE (-EBADF). That is a stale handle, not a denial.
 * Sovereign bypasses the owner test only; a stale handle still fails for it. */
#define PSOCK_DENIED (-2)
#define PSOCK_STALE  (-1)
/* DDR-988 sec.10: a genuine tcp_write() failure needs its OWN code. sec.10 had
 * psock_write return -1 for it, which IS PSOCK_STALE, so a caller could not
 * tell "your handle is dead" from "the send failed" -- and sys_sock_write
 * flattened both to -EIO, losing the -EBADF the read and close paths document
 * for a stale handle. */
#define PSOCK_EIO    (-3)

static int psock_resolve(int h, uint32_t owner, int sovereign,
                         struct proxy_sock **out) {
    if (h < 0)
        return PSOCK_STALE;
    struct proxy_sock *s = &g_ps[PSOCK_SLOT(h)];
    if (!sovereign && s->owner != owner)
        return PSOCK_DENIED;
    if (!s->used || s->gen != PSOCK_GEN(h))
        return PSOCK_STALE;
    *out = s;
    return 0;
}

/* Open a connection to host (big-endian a.b.c.d packed) : port. Returns slot. */
int psock_connect(uint32_t host, uint16_t port, uint32_t owner) {
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
        net_unlock(fl);
        pmm_free_page((uint64_t)(uintptr_t)rxpage);
        return -1;
    }
    struct proxy_sock *s = &g_ps[slot];
    s->rx = rxpage;
    s->head = s->tail = 0;
    s->used = 1;
    s->state = PS_CONNECTING;
    s->owner = owner;                 /* DDR-987 sec.10: claimed under the lock */
    s->gen   = g_psock_gen++;         /* monotonic; never reuses a live handle */
    s->host_be = host; s->port = port;   /* DDR-1070: audit identity of the peer */
    s->pcb = tcp_new();
    if (!s->pcb) {
        s->rx = 0; s->used = 0; s->state = PS_CLOSED; s->owner = 0;
        s->host_be = 0; s->port = 0;                    /* DDR-1070 */
        net_unlock(fl);        /* DDR-987 */
        pmm_free_page((uint64_t)(uintptr_t)rxpage);     /* free the page AFTER release */
        return -1;
    }
    tcp_arg(s->pcb, s);
    tcp_recv(s->pcb, ps_recv);
    tcp_err(s->pcb, ps_err);
    ip_addr_t ip;
    IP4_ADDR(&ip, (host >> 24) & 0xff, (host >> 16) & 0xff, (host >> 8) & 0xff, host & 0xff);
    err_t e = tcp_connect(s->pcb, &ip, port, ps_connected);
    if (e != ERR_OK) {
        /* DDR-987 sec.9: lwIP's contract is that a FAILED tcp_connect() did not
         * enqueue the attempt and the caller still owns the pcb -- only ERR_OK
         * transfers it to the stack (then the connected/err callback owns it).
         * The old path just set PS_ERR and returned, leaking the pcb, the slot
         * (used stayed 1) and the RX page. Roll the whole claim back under the
         * lock; free the page after release, as everywhere else here. */
        tcp_abort(s->pcb);
        s->pcb = 0; s->rx = 0; s->used = 0; s->state = PS_CLOSED; s->owner = 0;
        s->host_be = 0; s->port = 0;                    /* DDR-1070 */
        net_unlock(fl);
        pmm_free_page((uint64_t)(uintptr_t)rxpage);
        return -1;
    }
    int handle = PSOCK_H(s->gen, slot);             /* DDR-987 sec.10 */
    net_unlock(fl);        /* DDR-987 */
    return handle;
}

int psock_state(int h, uint32_t owner, int sovereign) {
    struct proxy_sock *s;
    uint64_t fl = spin_lock_irqsave(&g_net_lock);   /* DDR-987 sec.10 */
    int rc = psock_resolve(h, owner, sovereign, &s);
    if (rc == 0) rc = (int)s->state;
    net_unlock(fl);
    return rc;
}

/* Drain up to len bytes into kbuf. Returns bytes copied (0 if the ring is empty);
 * the caller distinguishes EOF/timeout via psock_state(). */
/* DDR-1070: the peer this handle is connected to, for the audit record on a
 * privacy-refused I/O. Goes through psock_resolve under the lock, so it can
 * never report a retired slot's destination (resolve requires `used` and a
 * matching generation) and it applies the same ownership rule as every other
 * operation. Returns 0 and fills the two out-params, or the psock error code. */
int psock_dest(int h, uint32_t owner, int sovereign,
               uint32_t *host_out, uint16_t *port_out) {
    struct proxy_sock *s;
    uint64_t fl = spin_lock_irqsave(&g_net_lock);
    int rc = psock_resolve(h, owner, sovereign, &s);
    if (rc == 0) { *host_out = s->host_be; *port_out = s->port; }
    net_unlock(fl);
    return rc;
}

int psock_read(int h, uint32_t owner, int sovereign, uint8_t *kbuf, int len) {
    struct proxy_sock *s;
    uint64_t fl;
    fl = spin_lock_irqsave(&g_net_lock);            /* DDR-987 */
    /* DDR-987 sec.10: authority AND liveness resolved under the same lock that
     * guards the operation, so a close+reuse on another cpu cannot slip between. */
    int rc = psock_resolve(h, owner, sovereign, &s);
    if (rc != 0 || !s->rx) {
        net_unlock(fl);
        return (rc != 0) ? rc : PSOCK_STALE;
    }
    int n = 0;
    while (n < len && s->tail != s->head) {
        kbuf[n++] = s->rx[s->tail];
        s->tail = (uint16_t)((s->tail + 1) & PSOCK_MASK);
    }
    net_unlock(fl);        /* DDR-987 */
    return n;
}

int psock_write(int h, uint32_t owner, int sovereign, const uint8_t *kbuf, int len) {
    struct proxy_sock *s;
    uint64_t fl;
    fl = spin_lock_irqsave(&g_net_lock);            /* DDR-987 */
    int rc = psock_resolve(h, owner, sovereign, &s);   /* DDR-987 sec.10 */
    if (rc != 0) {
        net_unlock(fl);
        return rc;
    }
    if (s->state != PS_OPEN || !s->pcb) {
        net_unlock(fl);
        return PSOCK_STALE;
    }
    u16_t snd = tcp_sndbuf(s->pcb);
    if (len > (int)snd) len = (int)snd;
    err_t e = ERR_OK;
    if (len > 0) {
        e = tcp_write(s->pcb, kbuf, (u16_t)len, TCP_WRITE_FLAG_COPY);
        if (e == ERR_OK) tcp_output(s->pcb);
    }
    net_unlock(fl);        /* DDR-987 */
    return (e == ERR_OK) ? len : PSOCK_EIO;   /* DDR-988 sec.10 */
}

int psock_close(int h, uint32_t owner, int sovereign) {
    struct proxy_sock *s;
    uint64_t fl;
    fl = spin_lock_irqsave(&g_net_lock);            /* DDR-987 */
    /* DDR-987 sec.9 + sec.10: resolve authority AND generation inside the lock.
     * sec.9 stopped cpu A tearing down cpu C's replacement socket after a
     * close+reuse; sec.10 additionally stops it when C is the same owner. */
    int rc = psock_resolve(h, owner, sovereign, &s);
    if (rc != 0) {
        net_unlock(fl);
        return rc;
    }
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
    /* DDR-988 sec.10: do NOT clear s->owner here. Clearing it left the slot
     * owned by nobody, so the caller that had just closed the handle failed the
     * OWNER check before ever reaching the generation check, and got
     * PSOCK_DENIED (-EPERM) for its own stale handle where the header promises
     * PSOCK_STALE (-EBADF). Retaining the retired owner until the slot is
     * reallocated gives exactly the documented split at no extra state: the
     * previous owner matches, then trips !used -> STALE; an unrelated caller
     * still mismatches -> DENIED. Safe because psock_reap_owner gates on `used`
     * (a retired slot is never re-closed) and psock_connect overwrites owner
     * when it reallocates. The connect-rollback sites above still clear it:
     * there the caller never received a handle at all. */
    s->rx = 0; s->used = 0; s->state = PS_CLOSED;
    net_unlock(fl);        /* DDR-987 */
    if (rxpage) pmm_free_page((uint64_t)(uintptr_t)rxpage);
    return 0;
}

/* DDR-987 sec.10 + DDR-731 exit reap. Owned by the port now that `owner` lives
 * on the slot: close every slot this pid holds. Each close re-enters the lock,
 * which is correct -- the lock is not held across the loop, so an ISR waiting on
 * it is never blocked for more than one teardown (sec.8's rule). */
void psock_reap_owner(uint32_t pid) {
    for (int i = 0; i < PSOCK_N; i++) {
        uint64_t fl = spin_lock_irqsave(&g_net_lock);
        int live = (g_ps[i].used && g_ps[i].owner == pid);
        int h = live ? PSOCK_H(g_ps[i].gen, i) : -1;
        net_unlock(fl);
        if (live)
            psock_close(h, pid, 0);
    }
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
    virtio_net_set_rx(pradyos_netif_rx_isr);     /* DDR-988 sec.3: RX IRQ -> deferred queue */
    tcp_echo_init();                             /* TCP echo on :8007 (smoke-net) */
    g_net_ready = 1;
    kputs("[net] lwIP up 10.0.2.15/24\r\n");
    /* DDR-987 sec.8: release BEFORE the self-tests. Holding across their pump
     * loops stalls every other cpu's timer ISR on this lock (see net_pump_locked).
     * The tests take the lock themselves, one iteration at a time. */
    net_unlock(fl);        /* DDR-987 */
    net_loopback_test();
    net_loopback_tcp_test();                     /* DDR-753: TCP client echo over loopback */
    net_fuzz_test();                             /* malformed-frame + SYN-flood hardening */
}

/* Called from the PIT tick handler. NOTE the real cadence: idt.c gates this on
 * (g_ticks % 10) == 0 and the PIT is 100 Hz, so lwIP is polled at ~10 Hz --
 * one opportunity per ~100 ms. DDR-987 sec.11 said 100 Hz / 10 ms in both its
 * text and this comment; both were wrong, and the error mattered, because it
 * understated the cost of a lost opportunity by 10x (DDR-988 sec.6). */
void net_poll_tick(void) {
    if (!g_net_ready)
        return;
    /* DDR-988. Publish the request BEFORE trying to serve it. If we get the lock
     * we run it inline via the drain; if we do not, the flag is already visible
     * to whichever cpu releases the lock next, and that release drains it. So a
     * contended tick is DEFERRED, never dropped -- which is what DDR-987 sec.11
     * got wrong: it assumed the holder was doing the timer work, and most
     * holders (psock_read/write/close, RX) do not run timers at all.
     *
     * Still a trylock, for the reason sec.11 was right about: this runs in the
     * timer ISR, and a cpu that blocks here spins with interrupts off and cannot
     * take its own next timer interrupt -- its per-cpu tick counter freezes and
     * virtio-blk's tick-bounded deadline expires ([vblk] compl wait timeout,
     * dest_dticks=0). That is the regression that reddened 23432af. */
    __atomic_store_n(&g_net_timer_pending, 1u, __ATOMIC_RELEASE);
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    if (spin_trylock(&g_net_lock)) {
        net_drain_locked();
        spin_unlock(&g_net_lock);
    } else {
        NET_CNT_INC(g_net_tick_skipped);   /* deferred, not lost (R17 denominator) */
    }
    __asm__ volatile("push %0; popfq" :: "r"(fl) : "memory", "cc");
}
