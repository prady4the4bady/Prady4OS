"""D-10 discriminating gate — experiment outcome evaluator."""

from __future__ import annotations

import inspect
from pathlib import Path

import pytest

from aether.agents.research.experiment_outcome_evaluator import (
    Comparator,
    ExperimentOutcomeEvaluator,
    FalsificationCondition,
    OutcomeError,
    Verdict,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _evaluator(tmp_path: Path, granted: bool = True, tap=None):
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("D-10", "experiment_outcome_evaluator.py", "CAP_WATCHDOG")
    return ExperimentOutcomeEvaluator(
        lockbox=lb, store_path=tmp_path / "outcomes.jsonl", audit=audit,
        alignment_tap=tap,
    )


#: "falsified when p99 latency >= 100ms"
_COND = FalsificationCondition("p99_ms", Comparator.GE, 100.0)


def test_verdict_cannot_be_supplied_by_the_caller(tmp_path: Path) -> None:
    """THE discriminator.

    D-07 forces a falsification condition to be declared before the run so the
    verdict cannot be renegotiated once the numbers arrive. An evaluator that
    accepts a verdict argument hands that renegotiation straight back.
    """
    params = inspect.signature(ExperimentOutcomeEvaluator.evaluate).parameters
    assert "verdict" not in params
    assert set(params) == {"self", "hyp_id", "condition", "metrics"}


def test_condition_firing_falsifies(tmp_path: Path) -> None:
    ev = _evaluator(tmp_path)
    out = ev.evaluate("h1", _COND, {"p99_ms": 140.0})
    assert out.verdict is Verdict.FALSIFIED
    assert "140.0" in out.rationale


def test_condition_not_firing_confirms(tmp_path: Path) -> None:
    ev = _evaluator(tmp_path)
    assert ev.evaluate("h1", _COND, {"p99_ms": 40.0}).verdict is Verdict.CONFIRMED


def test_boundary_is_the_declared_one(tmp_path: Path) -> None:
    """>= 100 means 100 falsifies. Off-by-one here silently flips verdicts."""
    ev = _evaluator(tmp_path)
    assert ev.evaluate("h1", _COND, {"p99_ms": 100.0}).verdict is Verdict.FALSIFIED
    assert ev.evaluate("h2", _COND, {"p99_ms": 99.999}).verdict is Verdict.CONFIRMED


def test_unmeasured_metric_is_inconclusive_not_confirmed(tmp_path: Path) -> None:
    """A run that never measured the metric has not confirmed anything.

    Reading 'the condition did not fire' as CONFIRMED is how an untested claim
    becomes an established one.
    """
    ev = _evaluator(tmp_path)
    out = ev.evaluate("h1", _COND, {"throughput": 900.0})
    assert out.verdict is Verdict.INCONCLUSIVE
    assert "not measured" in out.rationale


@pytest.mark.parametrize(
    ("comparator", "value", "expected"),
    [
        (Comparator.LT, 5.0, Verdict.FALSIFIED),
        (Comparator.LT, 20.0, Verdict.CONFIRMED),
        (Comparator.LE, 10.0, Verdict.FALSIFIED),
        (Comparator.GT, 20.0, Verdict.FALSIFIED),
        (Comparator.GE, 10.0, Verdict.FALSIFIED),
        (Comparator.EQ, 10.0, Verdict.FALSIFIED),
        (Comparator.NE, 10.0, Verdict.CONFIRMED),
    ],
)
def test_every_comparator(tmp_path: Path, comparator, value, expected) -> None:
    ev = _evaluator(tmp_path)
    cond = FalsificationCondition("m", comparator, 10.0)
    assert ev.evaluate("h", cond, {"m": value}).verdict is expected


def test_merkle_chain_detects_a_rewritten_outcome(tmp_path: Path) -> None:
    """I-05 — a rewritten experiment history must be detectable, not merely
    discouraged."""
    ev = _evaluator(tmp_path)
    ev.evaluate("h1", _COND, {"p99_ms": 140.0})
    ev.evaluate("h2", _COND, {"p99_ms": 40.0})
    ev.evaluate("h3", _COND, {"p99_ms": 10.0})
    root = ev.root()
    assert ev.verify_chain(root) is True
    assert ev.verify(1) is True

    # Someone edits an inconvenient falsification into a confirmation.
    ev._outcomes[0].verdict = Verdict.CONFIRMED
    assert ev.root() != root
    assert ev.verify_chain(root) is False


def test_chain_detects_a_dropped_outcome(tmp_path: Path) -> None:
    """Deletion is the other half of tampering, and it leaves no edited record."""
    ev = _evaluator(tmp_path)
    for i, v in enumerate((140.0, 40.0, 10.0)):
        ev.evaluate(f"h{i}", _COND, {"p99_ms": v})
    root = ev.root()
    ev._outcomes.pop(0)
    assert ev.verify_chain(root) is False


def test_appending_changes_the_root(tmp_path: Path) -> None:
    ev = _evaluator(tmp_path)
    ev.evaluate("h1", _COND, {"p99_ms": 1.0})
    first = ev.root()
    ev.evaluate("h2", _COND, {"p99_ms": 1.0})
    assert ev.root() != first
    assert ev.verify(0) is True and ev.verify(1) is True


def test_empty_chain_root_is_empty(tmp_path: Path) -> None:
    assert _evaluator(tmp_path).root() == ""


def test_proof_out_of_range_refused(tmp_path: Path) -> None:
    ev = _evaluator(tmp_path)
    ev.evaluate("h1", _COND, {"p99_ms": 1.0})
    with pytest.raises(OutcomeError, match="no outcome at seq"):
        ev.proof(7)


def test_non_numeric_metric_refused(tmp_path: Path) -> None:
    ev = _evaluator(tmp_path)
    with pytest.raises(OutcomeError, match="must be numeric"):
        ev.evaluate("h1", _COND, {"p99_ms": "fast"})       # type: ignore[dict-item]


def test_bool_metric_refused(tmp_path: Path) -> None:
    """True == 1 would pass a naive numeric check and evaluate as a latency."""
    ev = _evaluator(tmp_path)
    with pytest.raises(OutcomeError, match="must be numeric"):
        ev.evaluate("h1", _COND, {"p99_ms": True})         # type: ignore[dict-item]


def test_missing_hypothesis_id_refused(tmp_path: Path) -> None:
    ev = _evaluator(tmp_path)
    with pytest.raises(OutcomeError, match="must name its hypothesis"):
        ev.evaluate("", _COND, {"p99_ms": 1.0})


def test_non_mapping_metrics_refused(tmp_path: Path) -> None:
    ev = _evaluator(tmp_path)
    with pytest.raises(OutcomeError, match="must be a mapping"):
        ev.evaluate("h1", _COND, [1, 2, 3])                # type: ignore[arg-type]


def test_falsified_ids_feed_the_dead_end_registry(tmp_path: Path) -> None:
    """D-13 consumes this; D-07 then blocks regeneration of those hypotheses."""
    ev = _evaluator(tmp_path)
    ev.evaluate("dead", _COND, {"p99_ms": 500.0})
    ev.evaluate("alive", _COND, {"p99_ms": 5.0})
    ev.evaluate("unmeasured", _COND, {"other": 1.0})
    assert ev.falsified_ids() == ["dead"]


def test_outcomes_are_sequenced_and_queryable(tmp_path: Path) -> None:
    ev = _evaluator(tmp_path)
    ev.evaluate("h1", _COND, {"p99_ms": 1.0})
    ev.evaluate("h1", _COND, {"p99_ms": 200.0})
    assert [o.seq for o in ev.outcomes] == [0, 1]
    assert len(ev.for_hypothesis("h1")) == 2


def test_evaluation_is_audited_with_the_root(tmp_path: Path) -> None:
    """S5 — the audit records what happened; the chain proves the order."""
    ev = _evaluator(tmp_path)
    ev.evaluate("h1", _COND, {"p99_ms": 140.0})
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    rec = [e for e in entries if e["action"] == "outcome.evaluate"]
    assert len(rec) == 1
    assert rec[0]["gate_status"] == "falsified"
    assert rec[0]["extra"]["root"] == ev.root()


def test_capability_required(tmp_path: Path) -> None:
    ev = _evaluator(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_WATCHDOG"):
        ev.evaluate("h1", _COND, {"p99_ms": 1.0})


def test_alignment_tap_receives_steps(tmp_path: Path) -> None:
    events: list[str] = []
    ev = _evaluator(tmp_path, tap=lambda s, _d: events.append(s))
    ev.evaluate("h1", _COND, {"p99_ms": 1.0})
    ev.verify_chain(ev.root())
    assert {"D-10.evaluate", "D-10.verify_chain"} <= set(events)


def test_condition_describe_is_stable() -> None:
    assert _COND.describe() == "p99_ms >= 100.0"
