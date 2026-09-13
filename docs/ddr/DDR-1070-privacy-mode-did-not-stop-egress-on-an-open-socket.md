# DDR-1070 — Privacy mode did not stop egress on an open socket

**Status:** IMPLEMENTED + gated + M1/M2
**Date:** 2026-09-06
**Branch:** `dev/phase1-seyp3n`
**Supersedes nothing. Corrects the SCOPE of DDR-802, not its design.**

---

## 1. The defect, in one sentence

`aether_privacy_active()` is consulted in `sys_sock_connect` and **nowhere else**,
so an agent holding a proxy socket that was already open when the operator
switched privacy mode on keeps sending and keeps receiving.

Measured, not reasoned:

- `grep -n "privacy\|aether" third_party/lwip-port/lwip_port.c` returns
  **nothing at all**. The lwIP port has no idea privacy mode exists.
- `sys_sock_write` (`sys_socket.c:164`) is: bound `len`, `copyin`, `psock_write`.
  No privacy call.
- `sys_sock_read` (`sys_socket.c:181`) is: bound `len`, loop on `psock_read` /
  `psock_state` until data, EOF, error or deadline. No privacy call.
- `psock_write` (`lwip_port.c:703`) takes `g_net_lock`, resolves ownership,
  checks `PS_OPEN`, then `tcp_write` + `tcp_output`. No privacy call.

Three call sites of `netallow_check` exist and all three are inside
`sys_sock_connect` — the deny check and the two audit predicates. The I/O paths
consult **slot ownership only**.

## 2. Why this matters, and why it is not a theoretical gap

`sys_socket.c:93` states DDR-802's design in the source itself:

> *"privacy mode is that same operator's explicit instruction that nothing
> leaves. Honouring the bypass here would let the flag override the control the
> operator just set."*

Privacy mode is ordered **ahead of the DDR-800 sovereign bypass** precisely so
that it is unconditional — it is the one place DDR-802 overrides DDR-800, and
the source says so deliberately. A control built to be unconditional was not
applied to the operation where data actually leaves.

The exposure is concrete rather than hypothetical. Agents are granted `CAP_NET`
at spawn, and `agent_base.c`'s live branch connects to `10.0.2.2:11434` and then
**writes a prompt and reads a response over that socket**. An operator who
switches privacy mode on mid-conversation stops the *next* connect and does not
stop the conversation in flight — including whatever the agent had already
composed into the prompt.

This is the DDR-1046 shape: a control that cannot see the case it exists for.

## 3. Why the gate did not catch it — and this one is NOT the dead-arm class

`user/privacynettest.c` is a good probe, and saying so is load-bearing, because
the failure here is a different one and calling it vacuity would file it under
the wrong lesson. It runs **sovereign and with CAP_NET** on the stated reasoning
that "if privacy mode refuses this caller, it refuses everyone"; it uses two
destinations that discriminate different failures; it runs three phases
(off → on → off) because "a privacy mode that cannot be switched off is a
different defect"; and it asserts the **audit trail**, including the absence of
an `AR_SOVEREIGN_BYPASS` record, which is what proves the ordering rather than
mere existence. Every one of those arms discriminates. A mutant that deletes the
connect-path check fails it.

The defect is that **every one of its three phases calls `SYS_SOCK_CONNECT`
and nothing else** — four connect calls, no other socket operation. Its
coverage is *connect*; the feature's claim is *nothing leaves*. Nothing in the
tree recorded the difference between the two, so the gap read as covered.

Carry this as its own class, distinct from the dead arm: **a gate every one of
whose arms is live, whose scope is narrower than the claim the feature makes.**
The dead-arm question is "can this arm fail?"; this question is "does the set of
arms span the claim?" — and the second is not answered by mutation-testing the
first, because every mutant of the connect path is caught.

## 4. What was already ruled out

**It is not blocked on a missing subsystem** (the DDR-1038 shape) and it is not
a design refusal (the DDR-1068 shape). The check is two calls to a function that
already exists, on paths that already run.

**It is not covered elsewhere.** `AETHER_MODE_PRIVACY_ON` handling is two lines
in `aether_queue.c:54`; it sets a flag. Nothing reaps sockets, nothing walks the
slot table, nothing touches lwIP.

## 5. Fix

### 5.1 Where the check goes, and why NOT under `g_net_lock`

The check goes in `sys_sock_write` and `sys_sock_read`, in the syscall layer,
mirroring `sys_sock_connect`.

A reviewer who knows DDR-987 §10 will ask why it is not pushed down into
`psock_write`/`psock_read` under `g_net_lock`, since that is exactly what
DDR-987 did to the **ownership** check. The distinction is the substance:

- DDR-987's TOCTOU was about the **identity of the slot**. `sock_denied()` read
  `g_sock_owner[slot]` unlocked and `psock_*` ran afterwards, so another CPU
  could close and reuse the slot in between and the operation landed on a
  **different connection**. The thing checked could change meaning.
- Privacy mode is a **global flag**, not a property of a slot. There is no
  identity to change underneath it. The only race is the operator toggling the
  mode inside a one-syscall window, on a control whose granularity is a human
  decision.

So this does not reintroduce what DDR-987 removed, and pushing it down would
add an AETHER dependency to a port layer that currently has zero — a real
layering property, measured above (`grep aether lwip_port.c` → nothing).

**No nesting, and no new lock-order edge.** `psock_dest()` acquires and releases
`g_net_lock` entirely inside itself and returns before `aether_audit()` runs, and
the later `psock_write`/`psock_read` acquisition is sequential rather than
nested — so the audit path never holds the network lock, and there is no
lock-order question to answer.

### 5.2 The refusal is audited, and that requires the destination

DDR-801's rule is that an unrecorded decision is not an audit trail, and DDR-802
audits every privacy refusal as `AR_PRIVACY_BLOCKED` keyed on
`AETHER_DEST_ID(host, port)`. A write refusal has only a **handle**, and a
handle is not a destination — an audit line reading "blocked handle 19" is
strictly weaker evidence and does not join up with the connect records for the
same conversation.

`struct proxy_sock` therefore gains `host_be` / `port`, set in `psock_connect`
beside `s->owner` under the same lock, from the arguments the caller already
passed, and cleared wherever `s->owner` is cleared. `psock_dest()` returns them
so the syscall layer can audit the same DEST_ID the connect did. 8 slots × 8
bytes = 64 bytes of BSS.

Two alternatives were considered and rejected:

- **Read `s->pcb->remote_ip` / `remote_port`.** Works, needs no struct change —
  but it re-derives from lwIP's internal representation a value the caller
  handed us verbatim, and `psock_connect` builds the `ip_addr_t` by decomposing
  `host` with shifts (`lwip_port.c:652`), so the round trip is a conversion that
  can be got wrong for no gain.
- **Audit the handle.** Rejected per DDR-801 above.

### 5.3 Deliberately NOT done: the connection is not torn down

Privacy-on now refuses I/O. It does **not** close the socket, and no FIN or RST
is sent, so a stateful observer on the wire still sees an ESTABLISHED
connection. That is a real residual and it is stated rather than glossed.

The reason it is not done here is DDR-802's own phase-3 requirement: privacy
mode must be **releasable**, because "a privacy mode that cannot be switched off
is a different defect, not a stricter version of this one". Refusing I/O is
reversible — `PRIVACY_OFF` restores the same handle, which phase 4d asserts.
Destroying connections is not: privacy-off could not restore them, and the
operator's toggle would become asymmetric and destructive. Whether the operator
wants a destructive kill-switch is a **decision**, in the DDR-793 security-posture
class this project defers to the operator, not a gap for me to close.

## 6. `SYS_NET_REVOKE` — the row that led here, and what it is really worth

This work started on Group C's `SYS_NET_REVOKE` / CAP_NET policy reload row.
Two findings, and neither is "it is unbuilt".

**(a) It is a refusal, not a gap — the DDR-1068 §2 class, second instance.**
`sys_socket.c:36` records DDR-734's decision in the source:

> *"Bounded, append-only (**no runtime revocation surface** — policy changes are
> a config edit + reboot), installed by the sovereign daemon (SYS_NET_ALLOW)
> from /etc/aether/config net= lines BEFORE any agent spawns."*

`netallow_add` only appends; `g_net_allow_n` only increments; there is no clear
and no remove. `sys_socket_register()` registers five syscalls and no revoke.
The backlog row asks for the exact surface DDR-734 declined, and, as with `bg`
in DDR-1068, a row that lists a deliberate refusal as remaining work invites
shipping the thing a prior DDR refused.

**(b) A revoke built before this fix would have been the DDR-1059 shape.**
`netallow_check` is connect-only. So removing a rule would **not sever a live
connection** either: the agent keeps its socket and keeps using it. A syscall
called `SYS_NET_REVOKE` that leaves the traffic it names flowing looks
considerably stronger than it is — security theatre with a real audit line
behind it, which is exactly what DDR-1059 refused to ship for the signed ledger.

So the ordering is: **this fix is a prerequisite for a meaningful revoke, not a
substitute for one.** No revoke is built here, and the row stays refused pending
an operator decision, now with the reason measured rather than asserted.

## 7. Gate — and the vacuity check that shaped it

**No new gate. 177 unchanged.** The arms go on `smoke-privacy-netfilter`, where
the machinery already runs, for the reason DDR-1039 recorded in refusing
`smoke-readline`.

### 7.1 The obvious arm is vacuous, measured before it was written

The natural arm is "privacy on, write, assert it failed". On the existing
probe's destination (`192.0.2.1`, TEST-NET-1) **nothing routes**, so the connect
never leaves `PS_CONNECTING` and `psock_write` returns `PSOCK_STALE` → `-EBADF`
*whether or not the fix is present*. An arm asserting `rc < 0` passes on a
kernel with no privacy check on the write path at all. Fourth time this class
has been caught in design text rather than after shipping.

Two things follow, and both are load-bearing:

1. **The socket must be genuinely live.** Phase 4 uses `127.0.0.1:8007`, the
   in-kernel TCP echo server `net_init()` binds (`lwip_port.c:374`), the same
   endpoint `user/nethammer.c` drives at 20,000 connects with `conn_err=0` — so
   a loopback connect provably reaches `PS_OPEN`. Phase 4a writes `ping` and
   **reads it back**; an echo is something the probe cannot manufacture, so the
   round trip is what establishes that the socket carries data in both
   directions before anything is asserted about refusing it.
2. **The errno must be exact.** The arm asserts `-EPERM`, never `< 0`, because
   `-EBADF` from a dead socket is also negative.

### 7.2 Phase 4

| arm | action | asserts |
|---|---|---|
| 4a | connect loopback:8007, write `ping`, read back | `n == 4` and the echo returns `ping` — the socket is live, both directions |
| 4b | `SET_MODE(PRIVACY_ON)`, write on the SAME handle | exactly `-EPERM` |
| 4c | read on the same handle | exactly `-EPERM` |
| 4d | `SET_MODE(PRIVACY_OFF)`, write + read back | works again — the refusal neither latched nor broke the socket |
| 4e | audit | `AR_PRIVACY_BLOCKED` present for `DEST_ID(127.0.0.1, 8007)` |

4d is the analogue of the existing phase 3 and it is not decoration: without it,
"privacy refused the write" and "the write path is broken" are the same
observation. 4e is what proves §5.2's DEST_ID is real rather than a handle.

The allowlist row `127.0.0.1:8007` is seeded in the `privnet` spawn block
exactly as the `nethammer` block seeds it, for the reason `nethammer.c:25`
records: without it every connect returns an audited `-EPERM`, the probe touches
nothing, and it still reaches its sentinel.

## 8. Mutation proof

- **M1 = the pre-fix behaviour** (both call sites and the helper deleted, which
  is literally the pre-DDR-1070 code on those paths), not a synthetic defect —
  the DDR-1066 standard. Kernel `59c0e350f7569108`. **Measured:** `rc=2`,
  `PRIVACYNET FAIL: phase 4b: write on an OPEN socket still permitted with
  privacy ON`.

  **And the kernel itself witnesses the egress.** In the M1 capture the echo
  server prints `PRADYOS_NET_TCP_READY` (295), `PRADYOS_NET_TCP_OK` (303 — 4a's
  echo, the socket is live), then the phase-4b failure (306), then
  **`PRADYOS_NET_TCP_OK` again at 322** — 4b's bytes arriving at the far end
  *after* the operator switched privacy mode on, and after the probe had already
  declared the failure. That is the defect demonstrated rather than argued: on
  the fixed kernel the second `TCP_OK` is 4d's, i.e. it appears only once privacy
  is switched back off.

  The slot fields and `psock_dest()` stay present in M1 and are simply
  unreferenced, so the mutant isolates **the check** rather than the plumbing.
- **M2 = write-path only** (privacy checked in `sys_sock_write`, not
  `sys_sock_read`). Kernel `1022410e09917bbe`. **Measured:** `rc=2`,
  `PRIVACYNET FAIL: phase 4c: read on an OPEN socket still permitted with
  privacy ON` — it **passes 4b** and fails **4c alone**, which is what proves the
  two directions are independent arms rather than one arm counted twice. A
  single-direction fix would otherwise have shipped looking complete.

Restoring the tree rebuilds `2c4868b2f5f0d00a` **bit-for-bit** (verified), so
both mutants are bound to their own binaries and neither result can be confused
with the shipped one.

Both recorded against kernel hashes; shipped kernel `2c4868b2f5f0d00a`.

**`GLOBAL_FORBIDDEN` deliberately NOT touched (76 unchanged).** The defect is
deterministic and its own gate covers both directions, so DDR-1065's reasoning
applies rather than DDR-981/1049's: a failure here names itself through
`PRIVACYNET FAIL`, which is already a `FORBIDDEN_SENTINEL` on this gate.

## 9. NOT claimed

- **No open issue moves.** OPEN-1, OPEN-2, OPEN-12 and OPEN-13 are untouched;
  no scheduler, block-layer or panic-path behaviour changes.
- **The connection is not closed** (§5.3) — privacy-on refuses I/O and leaves
  the TCP connection established.
- **`SYS_NET_REVOKE` is not built** (§6) and the allowlist remains append-only.
- **No claim about the live Ollama branch being exercised** — it is not; the
  gate drives loopback.
- **Nothing about `sys_sock_close`**, which is deliberately still permitted
  under privacy mode: refusing close would strand slots and leak the very
  connections the operator wants stopped.

---

*DDR-1070. Cross-refs: DDR-802 (privacy mode, scope corrected), DDR-800/801
(sovereign bypass + allowed-by-policy audit), DDR-734 (append-only allowlist),
DDR-987 §10 (the TOCTOU that does NOT apply here), DDR-1046 (a control that
cannot see its own case), DDR-1059 (a control that looks stronger than it is),
DDR-1068 §2 (a row that asks for what a DDR refused).*
