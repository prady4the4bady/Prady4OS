# ADR-025: NET-B — lwIP TCP/IP over virtio-net

- **Status:** Accepted 2026-06-27 — design record for slice NET-B.
- **Date:** 2026-06-27
- **Phase:** Layer 4/5 networking (NET-B; follows NET-A virtio-net bring-up).
- **Relation to prior ADRs:** builds on **NET-A** (`kernel/drivers/net/virtio_net.c`
  — virtqueue bring-up + TX test), **ADR-014** (virtio transport), **ADR-003/007**
  (PMM/VMM), **ADR-009** (NCS capabilities). Bound by **ADR-021** (W^X). lwIP runs
  **in the kernel** (ring 0), `NO_SYS=1`, single-threaded.

> **Why an ADR?** NET-B adds an attack surface that parses untrusted bytes off
> the wire inside ring 0. The integration model, the memory model, and every
> security control must be decided before code so they are designed in, not
> bolted on.

---

## Decisions

### D1 — lwIP version (pinned)
**lwIP 2.2.1** (tag `STABLE-2_2_1_RELEASE`, the latest stable as of 2026-06),
vendored as a git submodule at `third_party/lwip` from the official mirror
`https://github.com/lwip-tcpip/lwip`. The exact commit is recorded when the
submodule is added (Step 1, in this file's addendum and the commit message).
The submodule is never modified; all port code lives in `third_party/lwip-port/`.

### D2 — Integration model: raw (callback) API, `NO_SYS=1`
Use lwIP's **raw API** (`tcp_*`, `udp_*`, `pbuf_*`) — lowest overhead, no thread
or mutex assumptions, exactly matching a single-core `NO_SYS` kernel. The
`netconn`/socket APIs (which assume an OS thread layer) are **not** used. No lwIP
OS layer (`sys_arch` mutex/sem/mbox) is needed beyond the `NO_SYS` stubs.

### D3 — Threading & timers
lwIP runs single-threaded in kernel context. **`sys_check_timeouts()` is called
from the PIT tick handler** — every 10 ticks (100 ms; lwIP's coarse timer
resolution) — guarded so it only runs once lwIP is initialised. RX is delivered
from the virtio-net IRQ (deferred to a tick-bounded poll, see D8). No reentrancy:
lwIP calls happen with interrupts masked or from the single tick/IRQ path.

### D4 — Memory model: lwIP heap backed by the kernel
- `MEM_LIBC_MALLOC=0`, `MEM_SIZE = 256 KiB`. lwIP's `mem_malloc`/`mem_free` are
  routed to the kernel allocator (`kmalloc`/`kfree`) via a port `mem` shim so
  network buffers come from the same SLAB/PMM pool the rest of the kernel uses
  (no second heap). `MEMP_MEM_MALLOC=0`: pools are statically sized (below) for
  determinism.
- `PBUF_POOL_SIZE = 128`, pbuf payload sized for a 1500-MTU + virtio-net header.

### D5 — IP stack scope
**IPv4 first.** IPv6 is **deferred** (`LWIP_IPV6=0`). DHCP **deferred** — static
address **10.0.2.15/24, GW 10.0.2.2** (QEMU user-mode network defaults, matching
the gates). TLS is Layer 7, deferred.

### D6 — Security decisions (all enforced; this is the binding part)
1. **Length-before-field validation.** lwIP validates every header length before
   field access (`IP_REASS_*`, `CHECKSUM_CHECK_*` on); the port's RX path **drops
   any frame shorter than `virtio_net_hdr + 14` (Ethernet)** before handing bytes
   to lwIP — no over-read. Malformed frames are dropped with a counted log line,
   never a panic (`smoke-net-fuzz`).
2. **No src trust.** Routing/delivery decisions come only from lwIP's netif/PCB
   tables; src IP/port in a packet are never used to pick a path.
3. **ICMP rate limit:** `ICMP_TTL`/error responses capped at **≤10 per second**
   via a port token-bucket wrapping `icmp_*` error emission (log:
   `ICMP_RATELIMITED`). Prevents reflection/amplification.
4. **TCP RST validation (RFC 5961):** `LWIP_TCP_RST_CHECK_SEQ` semantics — a RST
   is accepted only if its sequence is **within the current receive window**;
   otherwise a challenge ACK is sent. (lwIP 2.2 implements RFC 5961 challenge-ACK;
   we keep it enabled and assert it in the ADR.)
5. **SYN flood:** **SYN cookies on by default** (`LWIP_TCP_SYNCOOKIE` / a port
   listen-backlog guard): when half-open PCBs exceed `MEMP_NUM_TCP_PCB`, new SYNs
   are answered statelessly so the table cannot be exhausted (verified
   `smoke-net-fuzz` SYN burst → no crash).
6. **Privileged ports:** binding a port **< 1024 from ring 3 requires
   `CAP_NET_BIND` (a new NCS capability bit, RES_NET)**; the kernel socket-bind
   bridge checks it. Kernel-internal servers (the echo gates) bind directly and
   are not subject to this. (Ring-3 socket access itself is a later slice; the
   capability and check are defined now so the policy is fixed.)
7. **Packet budget:** the RX path processes at most **64 packets per tick**; the
   remainder stay queued for the next tick — bounds work per interrupt/tick so a
   flood cannot livelock the kernel.

### D7 — Driver bridge (NET-A → lwIP)
`kernel/drivers/net/virtio_net.c` is extended (NET-A only did a TX test) to expose:
- `int virtio_net_tx(const void *frame, uint32_t len)` — prepend the
  `virtio_net_hdr`, enqueue on the TX virtqueue, notify.
- `void virtio_net_set_rx(void (*cb)(const uint8_t *frame, uint32_t len))` — the
  RX IRQ, on a used RX buffer, strips the `virtio_net_hdr`, invokes `cb` with the
  Ethernet frame, then **re-arms** the buffer (NET-A dropped+never re-armed).
- `void virtio_net_mac(uint8_t out[6])` — MAC from device config.
`third_party/lwip-port/pradyos_netif.c` provides the lwIP `netif`:
`linkoutput` → `virtio_net_tx`; the registered RX cb → `pbuf` alloc →
`netif->input` (`ethernet_input`). MAC, MTU 1500, `NETIF_FLAG_ETHARP|BROADCAST`.

### D8 — RX delivery discipline
The virtio-net RX IRQ sets a "work pending" flag and (for the gates) drains up to
the 64-packet budget directly; lwIP input runs in this bounded loop. (A softirq/
bottom-half is a later refinement; the budget makes the direct path safe.)

### D9 — Build
lwIP is third-party: compiled `-w` (not `-Werror`) into `build/lwip/liblwip.a`
via `tools/build_lwip.sh` (mirrors `tools/build_musl.sh`); the kernel links it.
The **port layer** (`third_party/lwip-port/`) is first-party C under `-Werror`.
`make lwip` builds the archive; CI checks out submodules + builds it.

### D10 — Gates (exact)
- **`smoke-net-lo`** — lwIP **loopback** netif (127.0.0.1): a kernel UDP echo on
  127.0.0.1:7 sends to itself and receives back → serial `PRADYOS_NET_LO_OK`.
  No QEMU port-forward needed (chosen over host-driven UDP for determinism).
- **`smoke-net`** — kernel **TCP echo** on 0.0.0.0:8007; QEMU `hostfwd`
  18007→8007; host harness connects, sends `PRADYOS_NET_PROBE`, expects the echo;
  serial prints `PRADYOS_NET_TCP_OK`.
- **`smoke-net-fuzz`** — feed malformed frames (zero-length, truncated headers,
  bad ethertype, bad IP version, checksum mismatch) + a SYN burst + a 100-ping
  ICMP burst; assert **no panic**, `ICMP_RATELIMITED` present, kernel survives
  (boot self-tests after still pass).

## Deferred (tracked here + build_status)
IPv6, DHCP, TLS, ring-3 socket syscalls (the `CAP_NET_BIND` policy is defined but
the user-facing socket bridge is later), softirq RX bottom-half, full reassembly
tuning. The **FAT32 large-read** bug (ADR-024 §D5) is unrelated and stays open.

## Build order after NET-B
**Layer 6 — AETHER agent runtime** (ADR-026). Begins only after NET-B is CI-green.
