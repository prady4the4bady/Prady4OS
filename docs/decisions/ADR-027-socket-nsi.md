# ADR-027: ring-3 socket NSI (proxy sockets over in-kernel lwIP)

- **Status:** Accepted 2026-06-28 — design record for the socket NSI slice.
- **Date:** 2026-06-28
- **Phase:** Layer 6 follow-on (unblocks live AETHER agent inference).
- **Relation to prior ADRs:** builds on **ADR-025** (lwIP in-kernel, `NO_SYS=1`,
  raw API), **ADR-022** (NSI + copyin/copyout), **ADR-026** (AETHER agents).
  Bound by **ADR-021** (W^X). Supersedes nothing.

> **Why an ADR?** lwIP runs in ring 0; agents run in ring 3; today there is no
> path between them, so an agent cannot reach a model runner. This adds the
> minimal kernel surface to bridge that gap **without** moving lwIP — the
> placement decision (ADR-025) stands. The concurrency model (how ring-3 calls
> the non-reentrant `NO_SYS` stack safely) must be fixed before code.

---

## Decisions

### D1 — Proxy-socket model (not a userspace TCP stack)
The kernel owns the TCP connection; ring 3 holds only an opaque small-integer
handle. A fixed table of **8 proxy sockets** in kernel BSS, each = one lwIP TCP
PCB + a **4 KiB kernel RX ring** (PMM-pool allocated, not BSS — the low-mem image
cap) + a state. The agent never sees a `pbuf`, a PCB, or a kernel address. This
keeps the untrusted-parsing surface (HTTP/JSON) in ring 3 and the raw stack in
ring 0, exactly matching the AETHER trust model (ADR-026 D1).

### D2 — Four append-only NSI calls (39–42)
```
SYS_SOCK_CONNECT(host_be_u32, port)        -> fd(0..7) | -errno
SYS_SOCK_WRITE  (fd, buf, len)             -> bytes written | -errno
SYS_SOCK_READ   (fd, buf, len, timeout_ms) -> bytes read (0 = EOF/timeout) | -errno
SYS_SOCK_CLOSE  (fd)                       -> 0 | -errno
```
`host_be_u32` is the IPv4 address as `(a<<24)|(b<<16)|(c<<8)|d` (e.g. 10.0.2.2 =
`0x0A000202`). `fd` is the proxy-table slot index — a private socket handle, NOT a
POSIX fd (it is only valid with the `SYS_SOCK_*` calls). All buffers cross
`copyin`/`copyout`; a bad pointer is `-EFAULT`, never a panic.

### D3 — Concurrency: serialize via the syscall's own IF=0
lwIP (`NO_SYS`) is not reentrant. The stack is otherwise driven only from IRQ
context (RX IRQ → `netif input`; PIT tick → `sys_check_timeouts`). Syscalls run
with **IF=0** (the SYSCALL `SFMASK` clears IF), so a `SYS_SOCK_*` handler calling
`tcp_connect`/`tcp_write`/`tcp_close` cannot be preempted by an RX/PIT IRQ — the
lwIP call is atomic w.r.t. the stack. The RX recv callback (IRQ context) only
*buffers* into the proxy ring; it never runs while a socket syscall is inside
lwIP. No new lock is introduced; the existing single-core IF discipline is the
lock. `SYS_SOCK_READ` waits for inbound data with `sti; hlt; cli` between checks
so the PIT/RX IRQs can deliver (and the recv callback fill the ring) even when the
agent is the only runnable thread; the ring is read with IF masked.

### D4 — Flow control + no data loss
The recv callback buffers a whole `pbuf` only if the ring has room; if not it
returns `ERR_MEM` **without** freeing or `tcp_recved`-ing, so lwIP retains the
segment (`refused_data`) and re-delivers it later (driven by the PIT
`sys_check_timeouts`). `tcp_recved` is issued for accepted bytes, so the TCP
window reflects what the agent has actually buffered — a slow agent throttles the
sender instead of losing bytes.

### D5 — Live agent mode (ring 3, deferred item from ADR-026 now unblocked)
`user/agent_base.c` gains a live path (compiled in; taken when `AETHER_TEST_MODE
== 0`): `SYS_SOCK_CONNECT` to the Ollama endpoint, an HTTP/1.1 `POST
/api/generate` body, and a **hand-written** extractor for the JSON `"response"`
field (no external lib — find `"response":"`, copy until the next unescaped `"`).
On success it prints `PRADYOS_AGENT_LIVE_OK`. Test mode is unchanged (fixed
response, `PRADYOS_AGENT_VERIFIED`), so CI is unaffected.

### D6 — Gate stays out of CI (needs a real model)
`smoke-agent-live` requires a running Ollama, so it is **developer-run only**:
`make smoke-agent-live [OLLAMA_HOST=a.b.c.d]` rebuilds the agent with
`AETHER_TEST_MODE=0` (default host 10.0.2.2:11434 — the QEMU SLIRP gateway maps to
the developer's loopback) and boots; the gate greps `PRADYOS_AGENT_LIVE_OK`. CI
continues to run only the deterministic test-mode `smoke-aether*` gates.

## Security model
- No kernel object (pbuf/PCB/pointer) is ever exposed to ring 3 (D1).
- Bounded: 8 sockets, 4 KiB ring each, per-call length caps; exhaustion returns
  `-EMFILE`/short writes, never a panic.
- Every user buffer crosses `copyin`/`copyout` (ADR-022).
- Outbound connect only (no in-kernel `bind`/`listen` for ring 3 here); a future
  `CAP_NET` gate can restrict which processes may open sockets (left ungated in
  this slice — the only client is the AETHER agent).
- A socket syscall never leaks a kernel address into agent-visible output.

## Alternatives considered
- **Userspace lwIP / a full BSD socket layer** — rejected: huge surface, would
  contradict ADR-025's in-kernel placement; the proxy model is ~200 LOC.
- **Shared-memory ring mapped to the agent** — rejected for now: more plumbing
  and a wider trust surface than copyin/copyout for the expected small payloads.
- **Blocking via sched_block/unblock on socket events** — deferred: the
  `sti;hlt;cli` + timeout poll is simpler and sufficient for request/response
  HTTP; event-driven blocking can replace it later without an ABI change.
