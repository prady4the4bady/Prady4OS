# DDR-753 — TCP loopback echo self-test

**Status:** proposed (pre-code)
**Layer:** 3 / NET (lwIP port). Sibling of the UDP loopback gate (`smoke-net-lo`).

## Problem

The net stack has a UDP loopback echo gate (`PRADYOS_NET_LO_OK`) and a TCP echo
*server* on `:8007`, but nothing exercises the **TCP client path over loopback**
end-to-end — connect (3-way handshake), data, echo, all through the stack. TCP
correctness (handshake + data delivery + the loopback netif for TCP) is only
proven indirectly by the external `smoke-net` gate, which depends on SLIRP.

## Decision

Add `net_loopback_tcp_test()` to the lwIP port, run at `net_init` right after
`tcp_echo_init()` (the `:8007` listener is up) and the UDP loopback test, still
inside the IRQ-masked init window (lwIP is `NO_SYS`, non-reentrant):

- `tcp_new()` → `tcp_recv(cb)` → `tcp_connect(127.0.0.1:8007, connected_cb)`.
- `connected_cb` sets a flag and `tcp_write("ping")` + `tcp_output`.
- The existing echo server accepts and echoes; `recv_cb` verifies the echoed
  bytes are `"ping"` and sets `echoed`.
- A **bounded** pump loop (`netif_poll_all()` + `sys_check_timeouts()`, ≤ 200
  iterations) drives the synchronous loopback delivery until `echoed` or the cap.
  Loopback delivery is immediate (no retransmit timers needed), so this converges
  in a handful of iterations; the cap guarantees **no hang** — on the cap it
  prints the FAIL sentinel instead of looping.
- Prints `PRADYOS_NET_TCP_LO_OK` on success, `PRADYOS_NET_TCP_LO_FAIL` otherwise.

The client pcb is left resident (like the UDP test's rx pcb) — one bounded PCB.
The echo server's existing `PRADYOS_NET_TCP_READY`/`_OK` lines now also fire at
boot from this loopback connection; they are positive sentinels no gate forbids.

## Gate — `smoke-net-tcp-lo` (new; 89 → 90)

`EXTRA_SENTINEL=PRADYOS_NET_TCP_LO_OK`, `FORBIDDEN_SENTINEL=PRADYOS_NET_TCP_LO_FAIL`,
via `boot_test.sh` (which always attaches virtio-net). Deterministic: loopback is
in-guest, no external network, no timing dependence beyond the bounded pump.

## Non-goals

- No ring-3 TCP client API change (the proxy-socket NSI already exists); this is
  an in-kernel stack self-test, not a new syscall.
- No large-transfer / windowing / retransmit stress (that is the fuzz gate's
  neighbour); a single small echo proves the client path.
- No external-endpoint TCP (SLIRP-dependent) — loopback only, for determinism.
