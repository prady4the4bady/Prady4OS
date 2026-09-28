# DDR-1091 — CAP_NET's allowlist is enforced at CONNECT, and UDP has no connect

**Status:** assessment. **No code change; no gate; docs-only.**
**Date:** 2026-09-07

---

## 1. What was being assessed

Group C's row *"UDP send / raw socket API"*. DDR-1069's own build test — *a
feature that nothing shipping needs, exercised only by its own gate, is the wrong
item* — is what decides whether to build it, and that test was applied. But
applying it turned up something the row does not record and that is worth
carrying whether or not the door is ever built.

The row was already corrected on 2026-09-06 and is accurate as far as it goes:
UDP *transport* works and is gated (`lwip_port.c:317-329` binds port 7,
`udp_sendto`s to loopback, and `smoke-net-lo` requires the resulting
`PRADYOS_NET_LO_OK`); what is missing is the **ring-3 door**. It names two
inherited design questions from DDR-1069 — `struct proxy_sock` is TCP-shaped, and
`fork` ownership. **There is a third, it is larger than either, and it is about
enforcement rather than ergonomics.**

## 2. The measurement

`netallow_check()` — the CAP_NET allowlist — is called from **exactly three
places, and all three are inside `sys_sock_connect`** (`sys_socket.c:114`, `:135`,
`:154`; the function spans `:79`–`:203`). Tree-wide, the only other references
are the forward declaration and three calls in `main.c:1670-1672`, which are a
**kernel self-test of the predicate itself**, not an enforcement site.

`sys_sock_write` (`:204`) carries **no** `netallow_check`. It carries DDR-1070's
`privacy_refuses_io()` and the slot-ownership check, and nothing else.

**For TCP that is coherent, and deliberately so.** A connection's destination is
fixed once, at connect time, so checking it once is checking it for every byte
that follows — and DDR-1070 closed the remaining half of the story, which was
that the *privacy* switch could be thrown after a socket was already open.

## 3. The finding: a UDP door would need a different enforcement SHAPE

**A datagram names its destination per send.** There is no connect, so there is
no moment at which the existing check could run. A `SYS_UDP_SENDTO` would have to
consult the allowlist **on every send** — which is not a new call site for an
existing check so much as a different enforcement model, with its own cost on the
hot path and its own audit-record question (DDR-801's rule is that the record
states the decision actually made, so a per-send decision implies a per-send
record unless something deliberately aggregates them).

**And it would inherit DDR-1070's defect by default unless designed against it.**
That DDR's finding was precisely that a control checked in `connect` and nowhere
else does not cover the operation where data actually leaves. A UDP door added
without both checks wired independently reproduces exactly that shape, one
protocol over — and this time there is no connect to check at all, so the
"nowhere else" is the *only* place there is.

That is the third design question, and it is why this row is not simply *"add
five syscalls beside the five TCP ones"*.

## 4. Not built, and the reason is DDR-1069's test rather than this finding

Measured: **nothing shipping needs a ring-3 UDP door.** The six ring-3 network
consumers (`sovegresstest`, `privacynettest`, `agent_base`, `nethammer`,
`capnettest`, `egressaudittest`) all use the TCP proxy surface. A UDP door would
ship exercised by its own gate and by nothing else.

**§3 is a reason to design it carefully if it is ever built — it is not the
reason it is not built.** Those are separate, and conflating them would let a
future session read "blocked" where the truth is "not needed yet, and here is the
trap when it is."

## 5. NOT CLAIMED

* **No defect is reported and none exists.** `netallow_check` being connect-only
  is correct for the protocol it guards; DDR-1070 fixed the one gap that did
  exist, and this DDR does not reopen it.
* **The UDP transport is not broken** — it is compiled in, exercised and gated by
  `smoke-net-lo`. What is absent is a ring-3 door, which is not the same thing.
* **No syscall is designed here**, and no NSI number is reserved. §3 names the
  shape of a decision, not a decision.
* **`SYS_NET_REVOKE` is untouched** and stays refused for DDR-734's recorded
  reason, as DDR-1070 §6 left it.
* No code change, `kernel.bin` untouched, 179 gates unchanged,
  `GLOBAL_FORBIDDEN` 76, no open issue moves (OPEN-1/2/12/13 untouched).
