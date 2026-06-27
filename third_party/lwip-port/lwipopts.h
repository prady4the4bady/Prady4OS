/* third_party/lwip-port/lwipopts.h — PRADYOS lwIP configuration (NET-B, ADR-025).
 *
 * lwIP runs in the kernel, single-threaded, NO_SYS. Every security-relevant knob
 * is set explicitly (not defaulted). Memory is backed by the kernel allocator
 * (kmalloc) rather than large static pools, to honour the "big tables come from
 * the heap, not BSS" rule — so MEM_LIBC_MALLOC + MEMP_MEM_MALLOC are on and route
 * to pradyos_lwip_{malloc,free,calloc} (lwip-port shim). This refines DDR-NET-B's
 * "static pools" note: determinism is kept via the MEMP_NUM_* count caps below.
 */
#ifndef PRADYOS_LWIPOPTS_H
#define PRADYOS_LWIPOPTS_H

/* --- system / threading ------------------------------------------------- */
#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0   /* single core; lwIP calls are serialized */
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_TIMERS                 1

/* --- memory: route everything to the kernel heap ------------------------ */
#define MEM_LIBC_MALLOC             1
#define MEMP_MEM_MALLOC             1
void *pradyos_lwip_malloc(unsigned long size);
void  pradyos_lwip_free(void *p);
void *pradyos_lwip_calloc(unsigned long n, unsigned long size);
#define mem_clib_malloc(s)          pradyos_lwip_malloc(s)
#define mem_clib_free(p)            pradyos_lwip_free(p)
#define mem_clib_calloc(n, s)       pradyos_lwip_calloc((n), (s))
#define MEM_ALIGNMENT               8
#define MEM_SIZE                    (256 * 1024)   /* cap when LIBC_MALLOC is off */

/* --- pool count caps (determinism; ADR-025 §D4) ------------------------- */
#define MEMP_NUM_TCP_PCB            16
#define MEMP_NUM_TCP_PCB_LISTEN     8
#define MEMP_NUM_UDP_PCB            8
#define MEMP_NUM_RAW_PCB            4
#define PBUF_POOL_SIZE              128

/* --- protocols ---------------------------------------------------------- */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0   /* deferred (ADR-025 §D5) */
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_DHCP                   0   /* static IP (ADR-025 §D5) */
#define LWIP_AUTOIP                 0
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DNS                    0
#define LWIP_IGMP                   0
#define LWIP_NETIF_LOOPBACK         1   /* smoke-net-lo */
#define LWIP_HAVE_LOOPIF            1
#define LWIP_NETIF_HOSTNAME         0

/* --- TCP tuning (ADR-025 §D4/§D6) --------------------------------------- */
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (8 * 1024)
#define TCP_WND                     (8 * 1024)
#define LWIP_TCP_SACK_OUT           1
#define LWIP_TCP_KEEPALIVE          1

/* --- security hardening (ADR-025 §D6) ----------------------------------- */
#define LWIP_BROADCAST_PING         0
#define LWIP_MULTICAST_PING         0
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_TCP          1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_ICMP         1
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_ICMP           1
/* ISN / SYN-cookie entropy: a kernel PRNG (lwip-port). */
unsigned int pradyos_lwip_rand(void);
#define LWIP_RAND()                 (pradyos_lwip_rand())

/* --- callbacks / stats -------------------------------------------------- */
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          0

/* --- freestanding header control ---------------------------------------- */
#define LWIP_NO_STDINT_H            0   /* compiler-provided stdint.h is fine */
#define LWIP_NO_STDDEF_H            0
#define LWIP_NO_INTTYPES_H          1   /* no <inttypes.h> freestanding; cc.h defines *_F */
#define LWIP_NO_LIMITS_H            0
#define LWIP_NO_CTYPE_H             1
#define LWIP_DEBUG                  0
#define LWIP_NOASSERT               0

#endif /* PRADYOS_LWIPOPTS_H */
