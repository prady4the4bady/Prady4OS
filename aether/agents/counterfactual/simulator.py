"""C-06 — counterfactual simulator.

Before a high-risk action runs, simulate what happens if it is *not* taken and
what happens under each alternative, then recommend.

Two properties decide whether this is a safety mechanism or theatre:

* **The baseline — doing nothing — is always evaluated.** Omitting it means the
  simulator can only ever recommend acting, since inaction is never on the
  ballot. Most "should I do this?" questions are answered by comparing against
  leaving it alone.
* **Low confidence escalates instead of proceeding.** Below
  ``ESCALATION_THRESHOLD`` (0.6) the result is routed to Offline Safety Review
  (C-08) — that is **S3**, and it is why the simulator returns a *recommendation
  plus an escalation flag* rather than a bare verdict.

Scoring is by alignment with S1–S14: an alternative that scores well on outcome
but violates an invariant loses. Model access is injected, so the gate runs
deterministically without a daemon.
"""

from __future__ import annotations

import json
import time
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Callable

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.invariants.core_invariants import CORE_INVARIANTS
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "CounterfactualQuery",
    "OutcomePrediction",
    "CounterfactualResult",
    "Simulator",
    "SimulationError",
    "ESCALATION_THRESHOLD",
    "BASELINE_LABEL",
    "MAX_ALTERNATIVES",
]

DDR_ID = "C-06"
_FILE = "aether/agents/counterfactual/simulator.py"
CAPABILITY = "CAP_COUNTERFACTUAL"

DEFAULT_STORE = Path(__file__).resolve().parent / "simulations.jsonl"
ESCALATION_THRESHOLD = 0.6
MAX_ALTERNATIVES = 5
BASELINE_LABEL = "<no-action>"


class SimulationError(RuntimeError):
    """Simulation refused or malformed."""


@dataclass(slots=True)
class CounterfactualQuery:
    proposed_action: str
    context: dict[str, Any] = field(default_factory=dict)
    alternatives: tuple[str, ...] = ()
    query_id: str = field(default_factory=lambda: str(uuid.uuid4()))


@dataclass(slots=True)
class OutcomePrediction:
    """One branch of the counterfactual."""

    action: str
    predicted_outcome: str
    benefit: float                       # 0..1
    invariants_violated: tuple[str, ...] = ()

    @property
    def score(self) -> float:
        """Any invariant violation is disqualifying, not a penalty.

        A weighted penalty lets a sufficiently attractive outcome buy its way
        past a safety rule, which is exactly what an invariant is meant to
        prevent.
        """
        if self.invariants_violated:
            return 0.0
        return max(0.0, min(1.0, self.benefit))


@dataclass(slots=True)
class CounterfactualResult:
    query_id: str
    baseline_outcome: OutcomePrediction
    alternative_outcomes: list[OutcomePrediction]
    recommended_action: str
    confidence: float
    escalate: bool
    ts: float = field(default_factory=time.time)

    def to_json(self) -> str:
        payload = asdict(self)
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))


#: Predicts one branch. Injected so the gate needs no live model.
PredictFn = Callable[[str, dict[str, Any]], OutcomePrediction]

_INVARIANT_IDS = frozenset(inv.ident for inv in CORE_INVARIANTS)


class Simulator:
    """Evaluates the baseline and each alternative, then recommends."""

    def __init__(
        self,
        lockbox: Lockbox,
        predict_fn: PredictFn,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
        escalation_threshold: float = ESCALATION_THRESHOLD,
        escalate_fn: Callable[[CounterfactualResult], None] | None = None,
    ) -> None:
        self.lockbox = lockbox
        self.predict_fn = predict_fn
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self.escalation_threshold = float(escalation_threshold)
        self.escalate_fn = escalate_fn

    def simulate(self, query: CounterfactualQuery) -> CounterfactualResult:
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not query.proposed_action:
            raise SimulationError("a query needs a proposed action")
        if len(query.alternatives) > MAX_ALTERNATIVES:
            raise SimulationError(
                f"{len(query.alternatives)} alternatives exceeds {MAX_ALTERNATIVES}"
            )

        # The baseline is not optional: without "do nothing" on the ballot the
        # simulator can only ever recommend acting.
        baseline = self._predict(BASELINE_LABEL, query.context)

        branches: list[OutcomePrediction] = [self._predict(query.proposed_action, query.context)]
        for alt in query.alternatives:
            branches.append(self._predict(alt, query.context))

        ranked = sorted(
            [baseline, *branches], key=lambda p: (-p.score, p.action)
        )
        winner = ranked[0]
        runner_up = ranked[1] if len(ranked) > 1 else None

        # Confidence is the margin over the next best option. A field of
        # equally-good choices is a low-confidence situation regardless of how
        # attractive the winner looks in isolation.
        margin = winner.score - (runner_up.score if runner_up else 0.0)
        confidence = round(min(1.0, winner.score * (0.5 + 0.5 * margin)), 4)
        escalate = confidence < self.escalation_threshold

        result = CounterfactualResult(
            query_id=query.query_id,
            baseline_outcome=baseline,
            alternative_outcomes=branches,
            recommended_action=winner.action,
            confidence=confidence,
            escalate=escalate,
        )

        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(result.to_json() + "\n")
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="counterfactual.simulate",
            gate_status="escalated" if escalate else "decided",
            query_id=query.query_id, recommended=winner.action,
            confidence=confidence,
        )
        if escalate and self.escalate_fn is not None:
            self.escalate_fn(result)
        return result

    def _predict(self, action: str, context: dict[str, Any]) -> OutcomePrediction:
        try:
            prediction = self.predict_fn(action, context)
        except Exception as exc:
            raise SimulationError(
                f"prediction failed for {action!r}: {type(exc).__name__}: {exc}"
            ) from exc
        if not isinstance(prediction, OutcomePrediction):
            raise SimulationError(
                f"predict_fn returned {type(prediction).__name__}, "
                "expected OutcomePrediction"
            )
        unknown = sorted(set(prediction.invariants_violated) - _INVARIANT_IDS)
        if unknown:
            raise SimulationError(f"unknown invariant id(s): {unknown}")
        return prediction
