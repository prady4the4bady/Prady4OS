# DDR-794 — `CAP_NET_BROWSE` (1<<23): definition and blast-radius review

**Status:** design + review only. **No code.** This DDR satisfies CONFIRM-1
**gate (2)** for `aether/cloud_bridge/` (DDR-793).
**Date:** 2026-07-28
**Relates to:** ADR-009 (NCS capabilities), DDR-731 (`CAP_NET`), DDR-734 (egress
allowlist), ADR-027 (socket NSI), DDR-792 (ollama bridge), DDR-793 (cloud bridge
deferred).

## What this capability is, and what it is not

`CAP_NET_BROWSE` (bit 23) authorises **outbound requests to hosts the operator
has not individually allow-listed** — the open-web case.

It is deliberately a *separate* bit from `CAP_NET` (1<<2) rather than a widening
of it. `CAP_NET` today means "may open a proxy socket to a host on the
allowlist" (DDR-731 + DDR-734). That is a bounded, operator-enumerated set. Web
browsing is unbounded by construction: the whole point is reaching a host nobody
listed in advance.

Collapsing the two would be the actual danger. Every agent already granted
`CAP_NET` for local Ollama would silently acquire open-web reach the moment the
semantics widened, and nothing in the existing audit trail would mark the
difference — the same `SYS_SOCK_CONNECT` records would be emitted for both. A
new bit means an agent must be granted it explicitly, and an audit reader can
tell the two apart after the fact.

**Not in scope:** `CAP_NET_BROWSE` does not imply `CAP_SOVEREIGN`, does not
bypass the allowlist for *listed* hosts, and does not grant inbound listening.

## Blast radius — every path between a default agent and an outbound byte

Traced against the tree at this commit, not from memory.

### Kernel (x86_64)

| # | gate | file:line | what it enforces |
|---|---|---|---|
| 1 | thread spawns with no network authority | `kernel/proc/sched.c:368` — `t->is_net = 0` | A new thread has **no** egress by default. Authority is granted, never inherited by omission. |
| 2 | only the sanctioned agent path sets it | `kernel/main.c:687` — `ut->is_net = 1` | The single site that grants `CAP_NET`. Any new grant site is a blast-radius event. |
| 3 | `SYS_SOCK_CONNECT` authority check | `kernel/syscall/sys_socket.c:75` — `if (!current_thread->is_net && !current_thread->is_sovereign)` → audited `-EPERM` | Denial is audited (`AR_CAP_DENIED`) and the caller survives. |
| 4 | egress allowlist, deny-by-default | `kernel/syscall/sys_socket.c:44` `netallow_check()`, table `g_net_allow[NET_ALLOW_MAX]` (8 entries), `sys_socket.c:38` | Host+port must match a rule; `port 0` = any port on that host. **`is_sovereign` bypasses this.** |
| 5 | per-slot ownership | `sys_socket.c:64` `sock_denied()` | A process cannot drive another's socket unless sovereign. |
| 6 | socket NSI surface | `SYS_SOCK_CONNECT/WRITE/READ/CLOSE` = 39–42 (`kernel/syscall/syscall.h:53-56`) | The complete kernel egress surface. There is no other syscall that emits a byte. |

**Count: a default agent is four independent gates away from an outbound byte**
(no `is_net`, no allowlist entry, no socket slot, no sovereign flag).

### Python layer

| # | gate | file | what it enforces |
|---|---|---|---|
| 7 | no HTTP client may be imported under `aether/agents/` | `test_single_inference_path.py` (I-01/I-08) | Enforced as a test, so a new direct-transport import fails CI. |
| 8 | inference routes through D-03 only | `aether/ai_core/ensemble/ensemble_router.py` | Single audited chokepoint; `route.call` records model, tokens, latency, cost. |
| 9 | transport is confined to the bridge | `aether/ollama_bridge/transport.py` (DDR-792) | The only module permitted to speak the wire protocol. `transport` has no default, so nothing acquires one implicitly. |
| 10 | privacy check precedes the socket | `transport.py` `generate()` | Checked **before** the rate limiter and before any transport touch. |
| 11 | principal authority | `aether/capability/enforcement.py` (I-02) | Fail-closed: an unregistered operation is denied even for a sovereign principal. |
| 12 | spawn is single-pathed | `aether/daemon/coordinator.py` (I-10) | Only the daemon starts agents, and `assert_sole_spawn_path()` detects side channels. |

### Where `CAP_NET_BROWSE` would have to be checked

Adding the capability means adding checks at **exactly two** places, and nowhere
else:

1. **Kernel** — a new branch in `sys_sock_connect` (`sys_socket.c:75`): when the
   target does **not** match `netallow_check()`, permit it only if the thread
   holds `CAP_NET_BROWSE`. This deliberately keeps deny-by-default for
   `CAP_NET`-only threads and makes browse an *additional* escape, not a
   replacement path. Requires a new `is_net_browse` field in `struct tcb`
   (`sched.h`) — and per the recorded lesson, an explicit initialiser in
   `sched_create`, since `kmalloc` does not zero.
2. **Python** — an `OperationRule("net.browse", Tier.SOVEREIGN, {CAP_SOVEREIGN})`
   in the I-02 enforcer, checked by `cloud_bridge` before its transport call.

## Risks this review surfaces

**R1 — the sovereign bypass is total.** `is_sovereign` skips both the authority
check and the allowlist (`sys_socket.c:75`, and the DDR-734 comment at `:79`).
That is correct for operator diagnostics and wrong as a foundation for browse:
any component that runs sovereign already has unrestricted egress today. Before
`CAP_NET_BROWSE` ships, the set of sovereign threads needs its own audit — this
DDR does not enumerate it and should not pretend to.

**R2 — the allowlist is 8 entries.** `NET_ALLOW_MAX 8` (`sys_socket.c:37`). Fine
for a handful of local endpoints, structurally unable to express "the web". This
is further evidence the two capabilities must stay separate: browse cannot be
implemented by growing the list.

**R3 — no per-destination audit today.** `AR_CAP_DENIED` records the denial but
the allowlist match path is not itself audited per destination. For `CAP_NET`
against a fixed local host that is tolerable; for browse it is not — an
unbounded destination set with no per-destination record cannot be reviewed
afterwards. **Adding that audit is a precondition of the browse implementation**,
and it is a kernel change, not a Python one.

**R4 — privacy mode does not yet exist.** DDR-793 gate (3). `CAP_NET_BROWSE`
without an enforced privacy block means the only thing preventing egress is that
no code currently calls out.

## Verdict for CONFIRM-1 gate (2)

Gate (2) asks for *a DDR with blast-radius review*, which this is. It does
**not** unblock `cloud_bridge` on its own, and this DDR explicitly does not
claim the capability is safe to implement: **R1 and R3 must be resolved first**,
and both are kernel work. Recorded here so the next slice starts from evidence
rather than re-deriving the call graph.
