# DDR-NET-B — lwIP integration, concrete design

Detailed design record for **ADR-025** (NET-B). Implementation-level decisions;
no code lands before this + ADR-025 are committed.

## File plan
- `third_party/lwip` — submodule, lwIP 2.2.1 (pinned; never modified).
- `third_party/lwip-port/` (first-party, `-Werror`):
  - `lwipopts.h` — config (values below).
  - `arch/cc.h` — compiler abstraction (types, byte order, `LWIP_PLATFORM_*`,
    `LWIP_RAND`, format macros) for the freestanding kernel toolchain.
  - `arch/sys_arch.h` / `sys_arch.c` — `NO_SYS=1` stubs (`sys_now()` from
    `g_ticks*10`, no sem/mutex/mbox needed).
  - `mem_port.c` — `mem_malloc`/`mem_free`/`mem_calloc` → `kmalloc`/`kfree`.
  - `pradyos_netif.c` — the lwIP `netif` ⇄ virtio-net bridge (D7).
  - `net_secure.c` — ICMP token bucket + length-precheck helpers (D6).
- `kernel/net/net.c/.h` — kernel entry: `net_init()` (lwip_init, netif_add for
  the virtio device + the loopback path, set default, set up), `net_poll_tick()`
  (called from PIT), the UDP echo (`smoke-net-lo`) and TCP echo (`smoke-net`)
  servers. New subsystem dir `kernel/net/` (no flat files in `kernel/` root).
- `kernel/drivers/net/virtio_net.{c,h}` — extended with `virtio_net_tx`,
  `virtio_net_set_rx`, `virtio_net_mac` (ADR-025 §D7).
- `tools/build_lwip.sh`, `Makefile` `lwip` target, CI submodule + `make lwip`.

## lwipopts.h (production values — ADR-025 §D4/§D6)
```
NO_SYS                 1
SYS_LIGHTWEIGHT_PROT   0          /* single core, IF-masked critical sections */
LWIP_NETCONN           0
LWIP_SOCKET            0
MEM_LIBC_MALLOC        0          /* use mem_port.c -> kmalloc */
MEM_ALIGNMENT          8
MEM_SIZE               (256*1024)
MEMP_NUM_TCP_PCB       16
MEMP_NUM_TCP_PCB_LISTEN 8
MEMP_NUM_UDP_PCB       8
PBUF_POOL_SIZE         128
TCP_MSS                1460
TCP_SND_BUF            (8*1024)
TCP_WND                (8*1024)
LWIP_TCP_SACK_OUT      1
LWIP_TCP_KEEPALIVE     1
LWIP_ICMP              1
LWIP_BROADCAST_PING    0          /* hardening */
LWIP_MULTICAST_PING    0
CHECKSUM_CHECK_IP      1
CHECKSUM_CHECK_TCP     1
CHECKSUM_CHECK_UDP     1
CHECKSUM_GEN_IP        1
CHECKSUM_GEN_TCP       1
CHECKSUM_GEN_UDP       1
LWIP_NETIF_STATUS_CALLBACK 1
LWIP_NETIF_LINK_CALLBACK   1
LWIP_IPV6              0          /* deferred */
LWIP_DHCP              0          /* static IP */
LWIP_STATS             1          /* counters for the security/fuzz gate */
LWIP_NETIF_LOOPBACK    1          /* smoke-net-lo */
LWIP_HAVE_LOOPIF       1
```
Every security-relevant macro is set explicitly (not defaulted), with an inline
comment in `lwipopts.h`.

## RX/TX flow (ADR-025 §D7/§D8)
- **TX:** `pradyos_netif_linkoutput(netif, pbuf)` → copy the pbuf chain into a
  netbuf, `virtio_net_tx(frame, len)`.
- **RX:** virtio IRQ → for each used RX buf (≤64/tick budget): strip
  `virtio_net_hdr`, length-precheck (≥14), `pbuf_alloc(PBUF_RAW)` + copy,
  `netif->input(pbuf, netif)` (= `ethernet_input`), re-arm the netbuf.
- **Timers:** PIT tick (every 10 ticks) → `net_poll_tick()` → `sys_check_timeouts()`.

## Security implementation notes (ADR-025 §D6)
- ICMP rate limit: a 1-second token bucket (10 tokens) checked before lwIP emits
  any ICMP error; on exhaustion drop + `kputs("ICMP_RATELIMITED\r\n")` once/window.
- Length precheck in the RX cb before any lwIP call (no over-read).
- SYN cookies / listen backlog: rely on lwIP's RFC 5961 + listen PCB cap; the
  fuzz gate sends > `MEMP_NUM_TCP_PCB` SYNs and asserts survival.
- `CAP_NET_BIND` (RES_NET) added to NCS; the (later) socket bridge checks it.

## Gate harness (ADR-025 §D10)
- `smoke-net-lo`: pure in-kernel, no host I/O → grep `PRADYOS_NET_LO_OK`.
- `smoke-net`: QEMU `-netdev user,id=n0,hostfwd=tcp::18007-:8007`; host harness
  (`printf PRADYOS_NET_PROBE | nc 127.0.0.1 18007` or bash `/dev/tcp`) after a
  wait-for-readiness on the serial log; grep `PRADYOS_NET_TCP_OK`.
- `smoke-net-fuzz`: kernel self-test injecting malformed frames into the RX path
  + SYN/ICMP bursts; grep no `[panic]`, `ICMP_RATELIMITED` present.

## Step sequence (matches ADR-025)
0. ADR-025 + this DDR (commit). 1. submodule + port + `liblwip.a` compiles
(`make lwip`). 2. netif bridge + `net_init` + loopback UDP echo + `smoke-net-lo`.
3. TCP echo + `smoke-net`. 4. security/fuzz hardening + `smoke-net-fuzz`.
5. full regression + CI green.
