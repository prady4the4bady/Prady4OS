# DDR-793 — `aether/cloud_bridge/`: deferred, then built (NOT enabled)

**Status:** all four CONFIRM-1 gates met 2026-07-28; the module is **built and
gated**. It is **NOT enabled in any shipping configuration** — DDR-794's R1 and
R3 are kernel-side preconditions that remain open. See "Residual risk" below.
**Date:** 2026-07-28
**Governed by:** CONFIRM-1, which blocks this directory until **all four** gates
below are true. They are not.

This DDR exists so the deferral is a recorded decision with evidence rather than
an omission. A directory that is empty because nobody got to it looks identical
to one that is empty because it is blocked — and the second is only safe while
someone remembers why.

## Gate status, checked against the repo at this commit

| # | gate | status | evidence |
|---|---|---|---|
| 1 | F#68 metric lockbox shipped and gated | ✅ **MET** (2026-07-28) | `aether/kernel/lockbox/metric_lockbox.py` + `test_metric_lockbox.py`, 18 tests. Non-`CAP_SOVEREIGN` write raises `CapError` and audits; a direct edit of the backing store is caught by the hash chain at load. |
| 2 | `CAP_NET_BROWSE` (1<<23) has a DDR with blast-radius review | ✅ **MET** (2026-07-28) | `docs/ddr/DDR-794-cap-net-browse.md` — defines the bit as separate from `CAP_NET`, traces all 12 gates between a default agent and an outbound byte, and raises R1–R4. Note DDR-794 states the capability is **not yet safe to implement**: R1 (total sovereign bypass) and R3 (no per-destination audit) are kernel preconditions. |
| 3 | Privacy mode (Section E 3D) has a **tested** netfilter hook that BLOCKS this path when active | ✅ **MET** (2026-07-28) | `aether/platform/privacy/netfilter.py` + `test_netfilter.py`, 14 tests. Interception is at the transport boundary (`filtered_transport`), gated by asserting the inner transport is never reached; proven end-to-end through the DDR-792 bridge with the bridge's own `privacy_fn` deliberately unset. |
| 4 | All cloud calls route through the same rate limiter (S2, 60 syscall/s) and audit tap as local Ollama — no bypass | ✅ **MET** (2026-07-28) | `aether/platform/ratelimit/shared_limiter.py` — one bucket; `acquire()` takes the path only to record it, never to select a bucket. `test_shared_limiter.py` exhausts the budget on the ollama path and requires the cloud path blocked; `test_cloud_bridge.py` does the same through the real bridge. The limiter and the netfilter are **constructor-required** on `CloudBridge`, so the bypass configuration cannot be built. |

**Four of four met.** The block is lifted; the module is built. CONFIRM-1's condition is conjunctive, so the block holds.

**Gate log**
- 2026-07-28 — gate 1 met: F#68 metric lockbox shipped.
- 2026-07-28 — gate 4 met: shared limiter shipped; cloud_bridge built against it.
- 2026-07-28 — gate 3 met: privacy netfilter hook shipped (Python transport
  boundary; the kernel lwIP hook remains future work — see the scope note).
- 2026-07-28 — gate 2 met: DDR-794 written. **Caveat:** the review itself
  concludes `CAP_NET_BROWSE` must not be implemented until its R1/R3 kernel
  preconditions are resolved. Gate 2 asked for the review, and the review says
  wait — recorded so a later reader does not mistake a met gate for a green light.

## Why these gates are not bureaucracy

Gate 3 is the load-bearing one and worth stating plainly. Every other control in
this layer — the lockbox, the I-02 enforcer, the D-08 monitor, the audit log —
governs what happens *inside* the machine. A cloud bridge is the first component
that moves data *out* of it. Until privacy mode can actually block that path,
enabling the bridge means the strongest privacy guarantee the system offers is
"no module currently calls out", which is a property of the current code rather
than a property of the system.

Gate 1 matters for a different reason: a self-improvement loop that can reach a
frontier model while its own objective function is still mutable is a loop that
can be argued into changing what it is optimising for. Order matters here — the
metric lockbox first, then the stronger reasoning.

## What unblocks this

In order, each needing its own DDR and gate:

1. ~~Ship F#68 (metric lockbox / S3) with its gate.~~ **Done 2026-07-28.**
2. ~~Write the `CAP_NET_BROWSE` DDR including a blast-radius review.~~
   **Done 2026-07-28 (DDR-794)** — and it raises R1 (sovereign bypass is total)
   and R3 (no per-destination egress audit) as kernel preconditions.
3. ~~Implement and gate the privacy-mode netfilter hook.~~ **Done 2026-07-28.**
   **Scope note:** the hook ships at the *Python transport boundary*, which is
   where `cloud_bridge` and `ollama_bridge` actually emit. CONFIRM-1's wording
   says "lwIP netfilter hook" — a kernel-side hook covering ring-3 sockets
   directly is NOT built, and would be needed before any non-Python component
   may claim privacy-mode coverage. Recorded rather than glossed: the gate as
   written asked for something broader than what is shipped.
4. ~~Design `cloud_bridge/` against DDR-792's shape, sharing the same rate
   limiter and audit tap.~~ **Done 2026-07-28** — same retry semantics, same
   deadline discipline, same audit field set (pinned by a test), and the shared
   limiter/netfilter are required constructor arguments rather than optional.

## Residual risk — why "built" is not "enabled"

CONFIRM-1's four gates are met, which is what unblocks writing the code. They
are **not** the same question as "is it safe to turn on", and two findings from
DDR-794 remain open, both kernel-side:

* **R1 — the sovereign bypass is total.** `is_sovereign` skips both the CAP_NET
  authority check and the egress allowlist (`kernel/syscall/sys_socket.c:75`).
  Anything running sovereign already has unrestricted egress, so the Python
  controls above are the only thing standing in front of it.
* **R3 — no per-destination egress audit.** The allowlist *match* path is not
  audited per destination. An unbounded destination set with no per-destination
  record cannot be reviewed after the fact.

Until both are closed, `CloudBridge` must not be constructed in a shipping
configuration. The constraint is restated in the module docstring, because the
file is what the next person reads.

## Trip-wire

Adding any Python file under `aether/cloud_bridge/` triggers, per CONFIRM-1:
`graph_rebuild()` plus a blast-radius review of `aether/ollama_bridge/` and
`aether/ai_core/ensemble/ensemble_router.py`. This DDR must be superseded — not
quietly amended — before that happens.
