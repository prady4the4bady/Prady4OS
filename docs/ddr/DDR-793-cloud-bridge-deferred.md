# DDR-793 — `aether/cloud_bridge/` is DEFERRED

**Status:** deferred, by rule. **No design and no code.** The directory stays
`.gitkeep`-only.
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
| 2 | `CAP_NET_BROWSE` (1<<23) has a DDR with blast-radius review | **NOT MET** | The token appears only in the feature tables (`AETHER_MASTER_FEATURES.md:255`, `:318`, `:357`). No `docs/ddr/` entry defines it and no blast-radius review exists. |
| 3 | Privacy mode (Section E 3D) has a **tested** lwIP netfilter hook that BLOCKS this path when active | **NOT MET** | No netfilter hook exists in `kernel/`; the only occurrence of the concept is in the feature document. There is therefore nothing that could block the path, tested or otherwise. |
| 4 | All cloud calls route through the same rate limiter (S2, 60 syscall/s) and audit tap as local Ollama — no bypass | **NOT ASSESSABLE** | Depends on 1–3 and on DDR-792's bridge existing. Cannot be evaluated, let alone met. |

**One of four met.** CONFIRM-1's condition is conjunctive, so the block holds.

**Gate log**
- 2026-07-28 — gate 1 met: F#68 metric lockbox shipped (commit below).

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
2. Write the `CAP_NET_BROWSE` DDR including a blast-radius review over
   `net_allow`, `CAP_NET`, and the socket NSI (ADR-027) — item 8 of the
   change-checklist in `AETHER_MASTER_FEATURES.md:357` already requires this for
   any network-policy change.
3. Implement and gate the privacy-mode netfilter hook, with a test that proves
   it **blocks** an egress attempt rather than merely being installed.
4. Then, and only then, design `cloud_bridge/` against DDR-792's shape, sharing
   the same rate limiter and audit tap rather than getting its own.

## Trip-wire

Adding any Python file under `aether/cloud_bridge/` triggers, per CONFIRM-1:
`graph_rebuild()` plus a blast-radius review of `aether/ollama_bridge/` and
`aether/ai_core/ensemble/ensemble_router.py`. This DDR must be superseded — not
quietly amended — before that happens.
