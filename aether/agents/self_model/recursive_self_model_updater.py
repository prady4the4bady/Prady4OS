"""D-09 — recursive self-model updater.

Closes the loop B-15 left open: the system revises its model of itself from its
own observed outcomes, rather than waiting to be told.

That loop is also the most direct route to privilege escalation in the whole
layer, and the reason is worth stating plainly. Downstream consumers (C-05
boundary mapper, D-01 self-improvement, D-02 planner, D-12 prioritiser) treat
the self-model as ground truth about what the system may attempt. If the system
can write "I hold CAP_SOVEREIGN" into its own self-model, it has granted itself
that capability everywhere it matters without ever touching the lockbox — the
lockbox stays pristine and the audit log shows nothing but a routine model
update.

So **S1 is enforced here as a hard invariant, not a policy**: every proposed
revision is re-checked against the lockbox at apply time, and a revision that
would add or widen a capability the lockbox does not grant raises
:class:`CapError`. Applying the update is not the enforcement point — proposing
it already is, and the refusal is audited either way.

**S13**: nothing is inferred from an empty history. A revision needs evidence
attached, and confidence never moves on assertion alone.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable

from aether.agents.self_model.self_model import SelfModel
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox

__all__ = [
    "RevisionKind",
    "Revision",
    "RevisionResult",
    "RecursiveSelfModelUpdater",
    "SelfModelUpdateError",
    "CapError",
    "MIN_EVIDENCE",
]

DDR_ID = "D-09"
_FILE = "aether/agents/self_model/recursive_self_model_updater.py"
CAPABILITY = "CAP_SELF_MODEL_WRITE"

#: S13 — a revision below this many observations is not evidence, it is noise.
MIN_EVIDENCE = 3


class SelfModelUpdateError(ValueError):
    """Revision refused as malformed."""


#: S1 violations surface as the lockbox's own error type, so a caller that
#: already handles capability failures handles self-escalation too.
CapError = CapabilityError


class RevisionKind(str, Enum):
    ADD_CAPABILITY = "add_capability"
    RECORD_OUTCOME = "record_outcome"
    RECORD_LIMIT = "record_limit"


@dataclass(slots=True)
class Revision:
    """One proposed change to the self-model."""

    kind: RevisionKind
    name: str
    evidence: str
    ddr_id: str = ""
    capability: str = ""
    successes: int = 0
    failures: int = 0
    ts: float = field(default_factory=time.time)

    @property
    def observations(self) -> int:
        return self.successes + self.failures


@dataclass(slots=True)
class RevisionResult:
    revision: Revision
    applied: bool
    reason: str


class RecursiveSelfModelUpdater:
    """Revises the self-model from observed outcomes, under S1."""

    def __init__(
        self,
        lockbox: Lockbox,
        self_model: SelfModel,
        audit: AuditLog | None = None,
        alignment_tap: Callable[[str, dict[str, Any]], None] | None = None,
        min_evidence: int = MIN_EVIDENCE,
    ) -> None:
        self.lockbox = lockbox
        self.self_model = self_model
        self.audit = audit if audit is not None else AuditLog()
        self.alignment_tap = alignment_tap
        self.min_evidence = int(min_evidence)
        self._history: list[RevisionResult] = []

    def _emit(self, step: str, detail: dict[str, Any]) -> None:
        if self.alignment_tap is not None:
            self.alignment_tap(f"{DDR_ID}.{step}", detail)

    # -- proposal ------------------------------------------------------------
    def propose(self, revision: Revision) -> Revision:
        """Validate a revision and refuse self-escalation at proposal time.

        Refusing only at apply time would leave a window in which a capability
        the lockbox never granted is sitting in a queue looking legitimate.
        """
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not revision.name:
            raise SelfModelUpdateError("a revision needs a target name")
        if not revision.evidence.strip():
            # S13: an unevidenced revision is exactly the flattering self-report
            # B-15 exists to reject.
            raise SelfModelUpdateError(
                f"revision {revision.name!r} carries no evidence"
            )

        if revision.kind is RevisionKind.ADD_CAPABILITY:
            if not revision.ddr_id or not revision.capability:
                raise SelfModelUpdateError(
                    "a capability revision needs ddr_id and capability"
                )
            # S1, the hard invariant. The self-model may reflect what the
            # lockbox grants; it may never be the thing that grants it.
            if not self.lockbox.check(revision.ddr_id, revision.capability):
                self.audit.append_action(
                    ddr=DDR_ID, file=_FILE, action="self_model.escalation_refused",
                    gate_status="refused", target=revision.name,
                    claimed_ddr=revision.ddr_id, capability=revision.capability,
                )
                self._emit("escalation_refused", {
                    "name": revision.name, "capability": revision.capability,
                })
                raise CapError(
                    f"self-model revision would grant {revision.capability} to "
                    f"{revision.ddr_id}, which the lockbox does not hold — "
                    f"self-escalation refused"
                )

        if revision.kind is RevisionKind.RECORD_OUTCOME:
            if revision.successes < 0 or revision.failures < 0:
                raise SelfModelUpdateError("outcome counts must be >= 0")
            if revision.observations < self.min_evidence:
                raise SelfModelUpdateError(
                    f"{revision.observations} observations below the evidence "
                    f"floor {self.min_evidence}"
                )

        self._emit("propose", {"kind": revision.kind.value, "name": revision.name})
        return revision

    # -- application ---------------------------------------------------------
    def apply(self, revision: Revision) -> RevisionResult:
        """Propose then apply. The proposal check is not optional."""
        self.propose(revision)                      # re-checked, always

        if revision.kind is RevisionKind.ADD_CAPABILITY:
            self.self_model.claim_capability(
                revision.name, revision.ddr_id, revision.capability
            )
            reason = f"lockbox grants {revision.capability} to {revision.ddr_id}"
        elif revision.kind is RevisionKind.RECORD_OUTCOME:
            for _ in range(revision.successes):
                self.self_model.record_outcome(revision.name, True)
            for _ in range(revision.failures):
                self.self_model.record_outcome(revision.name, False)
            reason = (f"{revision.successes} successes, "
                      f"{revision.failures} failures")
        else:
            self.self_model.record_limit(revision.name, revision.evidence)
            reason = "limit recorded"

        result = RevisionResult(revision=revision, applied=True, reason=reason)
        self._history.append(result)
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="self_model.revision",
            gate_status="applied", kind=revision.kind.value,
            target=revision.name, reason=reason,
        )
        self._emit("apply", {"kind": revision.kind.value, "name": revision.name})
        return result

    def apply_many(self, revisions: list[Revision]) -> list[RevisionResult]:
        """Apply in order, stopping at the first refusal.

        Deliberately not best-effort: a batch containing an escalation attempt
        is not a batch to partially trust.
        """
        out: list[RevisionResult] = []
        for revision in revisions:
            out.append(self.apply(revision))
        return out

    # -- recursion -----------------------------------------------------------
    def reconcile(self) -> list[RevisionResult]:
        """Drop claims the lockbox no longer backs.

        The recursive half: a capability revoked in the lockbox must not linger
        in the self-model, or the system keeps attempting what it can no longer
        do and reads the resulting failures as bad luck.
        """
        self.lockbox.require(DDR_ID, CAPABILITY)
        stale: list[RevisionResult] = []
        for claim in self.self_model.claims:
            if self.lockbox.check(claim.ddr_id, claim.capability):
                continue
            name = claim.name
            limit = (f"capability {claim.capability} for {claim.ddr_id} is no "
                     f"longer granted")
            self.self_model.record_limit(name, limit)
            result = RevisionResult(
                revision=Revision(
                    kind=RevisionKind.RECORD_LIMIT, name=name, evidence=limit,
                ),
                applied=True, reason="revoked capability reconciled",
            )
            stale.append(result)
            self._history.append(result)
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="self_model.reconcile",
                gate_status="revoked", target=name, capability=claim.capability,
            )
        self._emit("reconcile", {"revoked": len(stale)})
        return stale

    @property
    def history(self) -> tuple[RevisionResult, ...]:
        return tuple(self._history)
