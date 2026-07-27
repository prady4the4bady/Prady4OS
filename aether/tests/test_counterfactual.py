"""C-06 discriminating gate — counterfactual simulator."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from aether.agents.counterfactual.simulator import (
    BASELINE_LABEL,
    CounterfactualQuery,
    CounterfactualResult,
    OutcomePrediction,
    SimulationError,
    Simulator,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _sim(
    tmp_path: Path,
    predict_fn,
    granted: bool = True,
    escalate_fn=None,
) -> Simulator:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("C-06", "simulator.py", "CAP_COUNTERFACTUAL")
    return Simulator(
        lockbox=lb, predict_fn=predict_fn,
        store_path=tmp_path / "simulations.jsonl", audit=audit,
        escalate_fn=escalate_fn,
    )


def _scored(scores: dict[str, float], violations: dict[str, tuple[str, ...]] | None = None):
    viol = violations or {}

    def predict(action: str, _ctx: dict[str, Any]) -> OutcomePrediction:
        return OutcomePrediction(
            action=action,
            predicted_outcome=f"outcome of {action}",
            benefit=scores.get(action, 0.0),
            invariants_violated=viol.get(action, ()),
        )

    return predict


def test_simulate_picks_best(tmp_path: Path) -> None:
    """The mandated gate: two alternatives, the higher-scoring one wins."""
    sim = _sim(tmp_path, _scored({BASELINE_LABEL: 0.1, "act-A": 0.9, "act-B": 0.4}))
    result = sim.simulate(
        CounterfactualQuery(proposed_action="act-A", alternatives=("act-B",))
    )
    assert result.recommended_action == "act-A"
    assert len(result.alternative_outcomes) == 2


def test_baseline_is_always_evaluated_and_can_win(tmp_path: Path) -> None:
    """The discriminator.

    Doing nothing must be on the ballot. A simulator that only ranks the
    proposed action and its alternatives can never recommend inaction, so it
    always advises acting — which defeats the point of a counterfactual.
    """
    sim = _sim(tmp_path, _scored({BASELINE_LABEL: 0.95, "act-A": 0.2, "act-B": 0.1}))
    result = sim.simulate(
        CounterfactualQuery(proposed_action="act-A", alternatives=("act-B",))
    )
    assert result.recommended_action == BASELINE_LABEL
    assert result.baseline_outcome.benefit == 0.95


def test_invariant_violation_is_disqualifying_not_a_penalty(tmp_path: Path) -> None:
    """A very attractive outcome must not buy its way past a safety rule."""
    sim = _sim(
        tmp_path,
        _scored(
            {BASELINE_LABEL: 0.3, "act-A": 1.0, "act-B": 0.5},
            violations={"act-A": ("S2",)},          # exfiltration
        ),
    )
    result = sim.simulate(
        CounterfactualQuery(proposed_action="act-A", alternatives=("act-B",))
    )
    assert result.recommended_action == "act-B"
    winner = [p for p in result.alternative_outcomes if p.action == "act-A"][0]
    assert winner.score == 0.0


def test_low_confidence_escalates(tmp_path: Path) -> None:
    """S3 — below the threshold the result routes to Offline Safety Review."""
    escalated: list[CounterfactualResult] = []
    sim = _sim(
        tmp_path,
        _scored({BASELINE_LABEL: 0.5, "act-A": 0.5}),
        escalate_fn=escalated.append,
    )
    result = sim.simulate(CounterfactualQuery(proposed_action="act-A"))
    assert result.escalate is True
    assert result.confidence < 0.6
    assert escalated and escalated[0].query_id == result.query_id


def test_a_clear_winner_does_not_escalate(tmp_path: Path) -> None:
    escalated: list[CounterfactualResult] = []
    sim = _sim(
        tmp_path,
        _scored({BASELINE_LABEL: 0.05, "act-A": 1.0}),
        escalate_fn=escalated.append,
    )
    result = sim.simulate(CounterfactualQuery(proposed_action="act-A"))
    assert result.escalate is False
    assert escalated == []


def test_a_field_of_equals_is_low_confidence(tmp_path: Path) -> None:
    """Confidence is the margin, not the winner's score in isolation."""
    tight = _sim(tmp_path, _scored({BASELINE_LABEL: 0.9, "act-A": 0.9, "act-B": 0.89}))
    result = tight.simulate(
        CounterfactualQuery(proposed_action="act-A", alternatives=("act-B",))
    )
    assert result.confidence < 0.6
    assert result.escalate is True


def test_capability_required(tmp_path: Path) -> None:
    sim = _sim(tmp_path, _scored({BASELINE_LABEL: 0.1, "act-A": 0.9}), granted=False)
    with pytest.raises(CapabilityError, match="CAP_COUNTERFACTUAL"):
        sim.simulate(CounterfactualQuery(proposed_action="act-A"))


def test_too_many_alternatives_refused(tmp_path: Path) -> None:
    sim = _sim(tmp_path, _scored({}))
    with pytest.raises(SimulationError, match="exceeds"):
        sim.simulate(
            CounterfactualQuery(
                proposed_action="a", alternatives=tuple(f"alt{i}" for i in range(9))
            )
        )


def test_empty_action_refused(tmp_path: Path) -> None:
    sim = _sim(tmp_path, _scored({}))
    with pytest.raises(SimulationError, match="needs a proposed action"):
        sim.simulate(CounterfactualQuery(proposed_action=""))


def test_unknown_invariant_id_refused(tmp_path: Path) -> None:
    """A typo'd invariant id must not silently mean 'no violation'."""

    def predict(action: str, _ctx: dict[str, Any]) -> OutcomePrediction:
        return OutcomePrediction(action, "o", 0.9, invariants_violated=("S99",))

    sim = _sim(tmp_path, predict)
    with pytest.raises(SimulationError, match="unknown invariant"):
        sim.simulate(CounterfactualQuery(proposed_action="act-A"))


def test_bad_predict_return_refused(tmp_path: Path) -> None:
    sim = _sim(tmp_path, lambda _a, _c: "not a prediction")
    with pytest.raises(SimulationError, match="expected OutcomePrediction"):
        sim.simulate(CounterfactualQuery(proposed_action="act-A"))


def test_failing_predictor_is_wrapped(tmp_path: Path) -> None:
    def explode(_a: str, _c: dict[str, Any]) -> OutcomePrediction:
        raise RuntimeError("model down")

    sim = _sim(tmp_path, explode)
    with pytest.raises(SimulationError, match="prediction failed"):
        sim.simulate(CounterfactualQuery(proposed_action="act-A"))


def test_results_persisted_and_audited(tmp_path: Path) -> None:
    sim = _sim(tmp_path, _scored({BASELINE_LABEL: 0.1, "act-A": 0.9}))
    sim.simulate(CounterfactualQuery(proposed_action="act-A"))
    lines = (tmp_path / "simulations.jsonl").read_text(encoding="utf-8").splitlines()
    assert len(lines) == 1
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(
        e["ddr"] == "C-06" and e["action"] == "counterfactual.simulate" for e in entries
    )
