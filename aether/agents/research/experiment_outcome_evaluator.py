"""D-10 — experiment outcome evaluator.

Decides what a run of a D-07 hypothesis actually showed, and makes that verdict
tamper-evident.

Two rules carry the design.

**A hypothesis is falsified by its own stated condition, not by disappointment.**
D-07 forces every hypothesis to declare a falsification condition up front,
precisely so the verdict cannot be renegotiated after the numbers arrive. The
evaluator therefore takes the observed metrics and the *recorded* condition and
computes the verdict from them — it never accepts a verdict as an argument. An
evaluator that could be told the answer is a rubber stamp.

**I-05: outcomes are chained.** Each recorded outcome is a Merkle leaf, and
:meth:`root` publishes the running root. Editing or dropping a past outcome
changes the root, so a rewritten experiment history is detectable rather than
merely discouraged — the append-only audit log (S5) records what happened, and
the chain proves nothing has been reordered underneath it.

Invariants: **S5** (append-only), **S7** (tamper-evident), **S9**
(deterministic — the verdict is arithmetic, not a model call).
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, Callable

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.integrity.merkle import merkle_proof, merkle_root, verify_proof
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "Comparator",
    "FalsificationCondition",
    "Verdict",
    "ExperimentOutcome",
    "ExperimentOutcomeEvaluator",
    "OutcomeError",
]

DDR_ID = "D-10"
_FILE = "aether/agents/research/experiment_outcome_evaluator.py"
CAPABILITY = "CAP_WATCHDOG"

DEFAULT_STORE = Path(__file__).resolve().parent / "outcomes.jsonl"


class OutcomeError(ValueError):
    """Evaluation refused."""


class Verdict(str, Enum):
    CONFIRMED = "confirmed"
    FALSIFIED = "falsified"
    INCONCLUSIVE = "inconclusive"


class Comparator(str, Enum):
    LT = "<"
    LE = "<="
    GT = ">"
    GE = ">="
    EQ = "=="
    NE = "!="


_APPLY: dict[Comparator, Callable[[float, float], bool]] = {
    Comparator.LT: lambda a, b: a < b,
    Comparator.LE: lambda a, b: a <= b,
    Comparator.GT: lambda a, b: a > b,
    Comparator.GE: lambda a, b: a >= b,
    Comparator.EQ: lambda a, b: a == b,
    Comparator.NE: lambda a, b: a != b,
}


@dataclass(slots=True)
class FalsificationCondition:
    """The machine-checkable form of D-07's ``falsification_condition``.

    "Falsified when ``metric comparator threshold`` holds." Stated before the
    run, evaluated after it, never adjusted in between.
    """

    metric: str
    comparator: Comparator
    threshold: float

    def holds(self, value: float) -> bool:
        return _APPLY[self.comparator](value, self.threshold)

    def describe(self) -> str:
        return f"{self.metric} {self.comparator.value} {self.threshold}"


@dataclass(slots=True)
class ExperimentOutcome:
    hyp_id: str
    verdict: Verdict
    metrics: dict[str, float]
    condition: str
    rationale: str
    seq: int = 0
    ts: float = field(default_factory=time.time)

    def to_json(self) -> str:
        payload = asdict(self)
        payload["verdict"] = self.verdict.value
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))

    def leaf(self) -> str:
        """The canonical bytes that go into the Merkle chain."""
        return self.to_json()


class ExperimentOutcomeEvaluator:
    """Evaluates experiment runs against their pre-registered conditions."""

    def __init__(
        self,
        lockbox: Lockbox,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
        alignment_tap: Callable[[str, dict[str, Any]], None] | None = None,
    ) -> None:
        self.lockbox = lockbox
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self.alignment_tap = alignment_tap
        self._outcomes: list[ExperimentOutcome] = []

    def _emit(self, step: str, detail: dict[str, Any]) -> None:
        if self.alignment_tap is not None:
            self.alignment_tap(f"{DDR_ID}.{step}", detail)

    # -- evaluation ----------------------------------------------------------
    def evaluate(
        self,
        hyp_id: str,
        condition: FalsificationCondition,
        metrics: dict[str, float],
    ) -> ExperimentOutcome:
        """Compute the verdict from the metrics and the recorded condition.

        Note what this signature does *not* accept: a verdict. The caller
        supplies observations; the condition decides.
        """
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not hyp_id:
            raise OutcomeError("an outcome must name its hypothesis")
        if not isinstance(metrics, dict):
            raise OutcomeError(
                f"metrics must be a mapping, got {type(metrics).__name__}"
            )
        for name, value in metrics.items():
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                raise OutcomeError(
                    f"metric {name!r} must be numeric, got {type(value).__name__}"
                )

        if condition.metric not in metrics:
            # The run did not measure what the hypothesis said it would. That
            # is INCONCLUSIVE — reporting it as confirmed is how an untested
            # claim becomes an established one.
            outcome = self._record(ExperimentOutcome(
                hyp_id=hyp_id, verdict=Verdict.INCONCLUSIVE,
                metrics=dict(metrics), condition=condition.describe(),
                rationale=f"metric {condition.metric!r} was not measured",
            ))
            return outcome

        value = float(metrics[condition.metric])
        if condition.holds(value):
            verdict = Verdict.FALSIFIED
            rationale = (f"{condition.metric}={value} satisfies "
                         f"{condition.describe()}")
        else:
            verdict = Verdict.CONFIRMED
            rationale = (f"{condition.metric}={value} does not satisfy "
                         f"{condition.describe()}")
        return self._record(ExperimentOutcome(
            hyp_id=hyp_id, verdict=verdict, metrics=dict(metrics),
            condition=condition.describe(), rationale=rationale,
        ))

    def _record(self, outcome: ExperimentOutcome) -> ExperimentOutcome:
        outcome.seq = len(self._outcomes)
        self._outcomes.append(outcome)
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(outcome.to_json() + "\n")
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="outcome.evaluate",
            gate_status=outcome.verdict.value, hyp_id=outcome.hyp_id,
            seq=outcome.seq, root=self.root(),
        )
        self._emit("evaluate", {"hyp_id": outcome.hyp_id,
                                "verdict": outcome.verdict.value})
        return outcome

    # -- I-05 integrity chain ------------------------------------------------
    def root(self) -> str:
        """Merkle root over every outcome, in order."""
        if not self._outcomes:
            return ""
        return merkle_root([o.leaf() for o in self._outcomes])

    def proof(self, seq: int) -> list[Any]:
        """Inclusion proof for one outcome against the current root."""
        if not 0 <= seq < len(self._outcomes):
            raise OutcomeError(f"no outcome at seq {seq}")
        return merkle_proof([o.leaf() for o in self._outcomes], seq)

    def verify(self, seq: int, root: str | None = None) -> bool:
        target = self.root() if root is None else root
        return verify_proof(self._outcomes[seq].leaf(), self.proof(seq), target)

    def verify_chain(self, claimed_root: str) -> bool:
        """S7 — does the recorded history still hash to ``claimed_root``?"""
        ok = self.root() == claimed_root
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="outcome.verify_chain",
            gate_status="ok" if ok else "tampered", claimed_root=claimed_root,
        )
        self._emit("verify_chain", {"ok": ok})
        return ok

    # -- reads ---------------------------------------------------------------
    @property
    def outcomes(self) -> tuple[ExperimentOutcome, ...]:
        return tuple(self._outcomes)

    def for_hypothesis(self, hyp_id: str) -> list[ExperimentOutcome]:
        return [o for o in self._outcomes if o.hyp_id == hyp_id]

    def falsified_ids(self) -> list[str]:
        """Hypotheses whose own condition fired — D-13's dead-end feed."""
        return sorted({o.hyp_id for o in self._outcomes
                       if o.verdict is Verdict.FALSIFIED})
