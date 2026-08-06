# DDR-845 — J-01…J-06 Phase-4 retrospective audit: findings

**Status:** accepted
**Date:** 2026-08-06
**Covers:** Group 2 item 12
**Nature:** an AUDIT, not a feature — the deliverable is verified findings, not code

## J-01 — Python layer under pytest

**VERIFIED, by CI.** `.github/workflows/ci.yml:120` runs
`python -m pytest -W error -x -q aether/tests/` in the `aether-layer` job, and
that job is **green** in run 31104672684. `-W error` means a warning fails the
run, which is the standard this project applies to the C build.

**Stated honestly:** pytest is not installed on this development machine, so the
local arm of this check could not be run. The claim rests on CI, which is the
project's ground truth anyway. The Python layer is 154 files across
`agents/ ai_core/ capability/ cloud_bridge/ daemon/ kernel/ ollama_bridge/ platform/`.

## J-02 — I-01…I-10 integration wiring

**VERIFIED as already recorded.** `AETHER_MASTER_FEATURES.md:360` marks
`I-01…I-10` COMPLETE, with each item pointing at a concrete module
(`aether/capability/enforcement.py`, `aether/daemon/coordinator.py`,
`aether/agents/safety/alignment_wiring.py`, …). Those files exist and are inside
the pytest tree covered by J-01.

## J-03 — SHA-256 audit chain purity

**VERIFIED, and the finding is a distinction worth recording: there are TWO
audit chains, and they are separate mechanisms.**

| chain | implementation | hashing |
|---|---|---|
| kernel audit log | `kernel/aether/aether_audit.c` (DDR-842) | `kernel/crypto/sha256.c`, pure C |
| host-side log | `aether/kernel/audit/audit_log.py` | Python `hashlib` |

The kernel chain never calls Python — it cannot; it runs in ring 0 before any
interpreter exists. The `hashlib` uses found in `aether/` are the **host** layer's
own hashing (`agentnet/mesh.py`, `contracts/agent_contract.py`,
`cloud_bridge/transport.py`, `kernel/audit/audit_log.py`).

**J-03's requirement — no Python `hashlib` in the KERNEL-audited chain — holds.**
The risk it guards against is a future refactor that routes kernel audit records
through the host logger; the two must stay distinct, and the naming
(`aether/kernel/...` for a *host* module) makes that confusion easy. Recorded so
the trap is visible.

## J-04 — S1–S14 vs S1–S8 reconciliation

**VERIFIED, already documented, and now cross-referenced to the new gate.**

`AETHER_MASTER_FEATURES.md:297` states it exactly: the Python layer's invariants
are **S1–S14** in `aether/kernel/invariants/core_invariants.py` and are
*"independent of Section H's S1–S8 … the numbering collides, the meanings do not,
and they must never be merged."*

They are genuinely different: Python **S4** is "never forge or alter audit log
entries"; kernel **S4** is "the human gate is structural". Same label, unrelated
claims.

**Consequence for the new gate (DDR-844):** `smoke-invariants` asserts the
**kernel** S1–S8 only. It says nothing about the Python S1–S14, which are
enforced by `core_invariants.py` under pytest. Anyone reading "invariants are
gated" must not conclude both sets are covered by one gate.

## J-05 — privacy-mode netfilter end-to-end

**VERIFIED by an existing shipped gate.** `smoke-privacy-netfilter` is registered
in `tools/ci/gate_shards.txt` and passes in CI (confirmed green in run
31094358972 among others). No new work required.

## J-06 — cloud bridge R1/R3 gating, no unaudited egress

**VERIFIED, and it independently confirms the DDR-843 deferral of
`ACTION_BROWSE_WEB`.**

`aether/cloud_bridge/__init__.py` and `transport.py` state the module is *"built
and gated; NOT enabled by default"*, because DDR-794 found two unresolved
kernel-side risks:

- **R1** — the sovereign bypass is total
- **R3** — the allowlist match path is not audited per destination

So the bridge is not merely unused, it is unusable-by-policy until R1 and R3 are
closed. That is the same conclusion DDR-843 reached from the release side, from
different evidence: enabling `ACTION_BROWSE_WEB` would require closing two open
security gaps first, not flipping a switch.

## Outcome

All six audit items verified. **No code was written for this item**, which is the
correct outcome for an audit: the deliverable is knowing where the project
actually stands, and inventing work to make an audit feel productive would defeat
it.

Two findings are carried forward as live hazards rather than closed boxes: the
two-audit-chain naming trap (J-03) and the colliding invariant numbering (J-04).
