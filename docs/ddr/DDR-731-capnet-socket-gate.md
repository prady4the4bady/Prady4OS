# DDR-731 — CAP_NET: authority + ownership on the socket NSI

**Status:** proposed (pre-code)
**Layer:** 6/7 boundary (AETHER authority model over the ADR-027 socket NSI)
**Extends:** ADR-027 (ring-3 proxy sockets), ADR-026 (AETHER authority flags).

## Problem

The socket NSI (`kernel/syscall/sys_socket.c`, NSI 39–42) has **no authority
check and no ownership**:

1. **Any process can reach the network.** `SYS_SOCK_CONNECT` is open to every
   ring-3 caller — a compromised or buggy CAP-less process can open outbound TCP
   to any host, bypassing the entire AETHER arbitration model (an agent's
   *actions* are queued and approved, but its *network* was never gated).
2. **Slots are global and cross-process.** The fd returned is an index into the
   8 kernel proxy slots; `SYS_SOCK_WRITE/READ/CLOSE` accept any index from any
   caller, so process A can read, inject into, or close process B's connection.
3. **Slots leak on exit.** Nothing closes a process's sockets when it dies; 8
   slots exhaust quickly (the same lifecycle hole DDR-729 closed for surfaces).

Only `user/agent_base.c` (live mode, Ollama HTTP) legitimately uses the NSI —
`smoke-net`'s TCP echo is kernel-side lwIP, unaffected.

## Decision

**New capability `CAP_NET`** — a kernel-set tcb flag `is_net` (like
`is_agent`/`is_sovereign`; explicitly zeroed in `sched_create_state` per the
tcb-fields-not-zeroed rule, and NOT inherited across fork by default — a fork
child re-earns authority like every other flag).

- **Grant:** kmain's agent spawn hook grants `is_net = 1` alongside `is_agent`
  (agents are the sanctioned network users — live mode talks to Ollama). The
  sovereign daemon/compositor path does NOT get an implicit grant; sovereignty
  is mode/approval authority, not network authority — `is_sovereign` callers
  pass the check only because the operator daemon may need diagnostics
  (`is_net || is_sovereign`).
- **Check:** `SYS_SOCK_CONNECT` requires `is_net || is_sovereign`; denial is
  `-EPERM` + an `AR_CAP_DENIED` audit entry (the AETHER pattern: audited, not
  fatal).
- **Ownership:** `sys_socket.c` records `g_sock_owner[slot] = pid` at connect.
  `WRITE/READ/CLOSE` require `owner == caller || is_sovereign`; `-EPERM`
  otherwise. Owner is cleared on close.
- **Exit reap:** `socket_reap_pid(pid)` (called from `sched_exit`, next to
  `surface_reap_pid`) closes every slot the exiting pid owns — one owner, one
  free point, no leak.

## Gate — `smoke-capnet` (77 gates)

Freestanding probe `user/capnettest.c` (musl-free, `user.ld`, no writable
globals) spawned CAP-less:

1. `SYS_SOCK_CONNECT` → must be `-EPERM` (not `-EMFILE`/success) →
   `CAPNET_CONNECT_DENIED`.
2. `SYS_SOCK_WRITE` and `SYS_SOCK_CLOSE` on slot 0 (never ours) → `-EPERM` →
   `CAPNET_SLOT_DENIED`.
3. Both → `PRADYOS_CAPNET_OK`; any unexpected result prints `CAPNET FAIL` and
   exits 1.

Regression: `smoke-net`, `smoke-net-lo`, `smoke-net-fuzz` (kernel-side lwIP,
must stay green), `smoke-aether*`, `smoke-agents`, `smoke-agentmetrics` (agent
spawn path now also grants `is_net`), then the full suite. `smoke-agent-live`
(developer-run) keeps working — the live agent holds `CAP_NET` by grant.

## Non-goals

- No per-host/per-port policy (a CAP_NET holder may connect anywhere) — a
  future allowlist slice if needed.
- No socket passing between processes.
- No change to the kernel-side lwIP echo/loopback tests.
