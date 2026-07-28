"""D-06 discriminating gate — causal reasoning engine."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from aether.agents.causal.causal_reasoning_engine import (
    MAX_OBSERVATIONS,
    CausalError,
    CausalReasoningEngine,
    LinkKind,
    Observation,
    correlation,
    partial_correlation,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _engine(
    tmp_path: Path, granted: bool = True, tap=None, max_obs: int = MAX_OBSERVATIONS
) -> CausalReasoningEngine:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("D-06", "causal_reasoning_engine.py", "CAP_CAUSAL_WRITE")
    return CausalReasoningEngine(
        lockbox=lb, store_path=tmp_path / "causal_graph.jsonl", audit=audit,
        alignment_tap=tap, max_observations=max_obs,
    )


def test_confounded_pair_is_not_reported_causal(tmp_path: Path) -> None:
    """THE discriminator: a common cause must not read as a direct cause.

    ice_cream and drownings both follow temperature. They correlate strongly
    with each other, so any engine promoting correlation to causation reports
    ice_cream -> drownings. Conditioning on temperature collapses it.
    """
    eng = _engine(tmp_path)
    for t in range(12):
        temp = float(t)
        eng.observe(
            Observation(
                values={
                    "temperature": temp,
                    "ice_cream": temp * 2.0 + 1.0,
                    "drownings": temp * 3.0 + 2.0,
                }
            )
        )
    # They really are strongly correlated — the trap is real.
    xs = [o.values["ice_cream"] for o in eng.observations]
    ys = [o.values["drownings"] for o in eng.observations]
    assert abs(correlation(xs, ys)) > 0.99

    link = eng.infer("ice_cream", "drownings")
    assert link.kind is LinkKind.CONFOUNDED, f"got {link.kind}: {link.mechanism}"
    assert link.confounder == "temperature"
    assert eng.graph.causal_links() == []


def test_intervention_promotes_to_causal(tmp_path: Path) -> None:
    """Only evidence correlation cannot supply may promote a link."""
    eng = _engine(tmp_path)
    for t in range(10):
        eng.observe(
            Observation(values={"switch": float(t), "lamp": float(t) * 5.0},
                        intervened_on="switch")
        )
    link = eng.infer("switch", "lamp")
    assert link.kind is LinkKind.CAUSAL
    assert "interventional" in link.mechanism
    assert link.strength > 0.9


def test_observational_correlation_stays_correlated(tmp_path: Path) -> None:
    """Correlated + survives screening + never intervened on != causal."""
    eng = _engine(tmp_path)
    for t in range(10):
        eng.observe(Observation(values={"a": float(t), "b": float(t) * 4.0 + 1.0}))
    link = eng.infer("a", "b")
    assert link.kind is LinkKind.CORRELATED
    assert "no intervention" in link.mechanism
    assert eng.graph.causal_links() == []


def test_independent_variables_are_independent(tmp_path: Path) -> None:
    eng = _engine(tmp_path)
    pattern = [1.0, -1.0, 1.0, -1.0, 1.0, -1.0, 1.0, -1.0]
    for i, v in enumerate(pattern):
        eng.observe(Observation(values={"noise": v, "ramp": float(i)}))
    assert eng.infer("noise", "ramp").kind is LinkKind.INDEPENDENT


def test_too_few_observations_is_not_a_claim(tmp_path: Path) -> None:
    """Under the evidence floor the engine must decline, not guess."""
    eng = _engine(tmp_path)
    for t in range(3):
        eng.observe(Observation(values={"a": float(t), "b": float(t) * 2}))
    link = eng.infer("a", "b")
    assert link.kind is LinkKind.INDEPENDENT
    assert "required" in link.mechanism


def test_prediction_uses_causal_links_only(tmp_path: Path) -> None:
    """Acting on a confounded link is the harm this engine prevents."""
    eng = _engine(tmp_path)
    for t in range(12):
        temp = float(t)
        eng.observe(Observation(values={"temperature": temp, "ice_cream": temp * 2,
                                        "drownings": temp * 3}))
    for t in range(10):
        eng.observe(Observation(values={"switch": float(t), "lamp": float(t) * 5},
                                intervened_on="switch"))
    eng.infer("ice_cream", "drownings")
    eng.infer("switch", "lamp")

    assert [l.effect for l in eng.predict_intervention("switch")] == ["lamp"]
    assert eng.predict_intervention("ice_cream") == []


def test_self_causation_refused(tmp_path: Path) -> None:
    eng = _engine(tmp_path)
    with pytest.raises(CausalError, match="cannot cause itself"):
        eng.infer("a", "a")


def test_empty_observation_refused(tmp_path: Path) -> None:
    eng = _engine(tmp_path)
    with pytest.raises(CausalError, match="at least one variable"):
        eng.observe(Observation(values={}))


def test_observation_window_is_bounded_and_drops_are_logged(tmp_path: Path) -> None:
    """S2 bounded + S12 no silent loss."""
    eng = _engine(tmp_path, max_obs=5)
    for t in range(8):
        eng.observe(Observation(values={"a": float(t)}))
    assert len(eng.observations) == 5
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    drops = [e for e in entries if e["action"] == "causal.observation_dropped"]
    assert len(drops) == 3
    assert drops[0]["extra"]["reason"]


def test_alignment_tap_receives_every_step(tmp_path: Path) -> None:
    """I-09 stub wire — every loop step emits, so D-08 drops in unchanged."""
    events: list[tuple[str, dict[str, Any]]] = []
    eng = _engine(tmp_path, tap=lambda step, detail: events.append((step, detail)))
    for t in range(6):
        eng.observe(Observation(values={"a": float(t), "b": float(t) * 2}))
    eng.infer("a", "b")
    eng.predict_intervention("a")
    steps = {s for s, _ in events}
    assert steps == {"D-06.observe", "D-06.infer", "D-06.predict"}


def test_capability_required(tmp_path: Path) -> None:
    eng = _engine(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_CAUSAL_WRITE"):
        eng.observe(Observation(values={"a": 1.0}))


def test_missing_variable_rows_are_skipped_not_guessed(tmp_path: Path) -> None:
    """A row lacking a variable must not be imputed."""
    eng = _engine(tmp_path)
    for t in range(6):
        eng.observe(Observation(values={"a": float(t), "b": float(t) * 2}))
    for t in range(6):
        eng.observe(Observation(values={"a": float(t)}))       # no 'b'
    link = eng.infer("a", "b")
    assert link.kind in (LinkKind.CORRELATED, LinkKind.CAUSAL, LinkKind.CONFOUNDED)


@pytest.mark.parametrize(
    ("xs", "ys", "expected"),
    [
        ([1.0, 2.0, 3.0], [2.0, 4.0, 6.0], 1.0),
        ([1.0, 2.0, 3.0], [6.0, 4.0, 2.0], -1.0),
        ([1.0, 1.0, 1.0], [1.0, 2.0, 3.0], 0.0),   # constant column, no NaN
        ([1.0], [2.0], 0.0),                        # too short
        ([], [], 0.0),
    ],
)
def test_correlation_edge_cases(xs, ys, expected) -> None:
    assert correlation(xs, ys) == pytest.approx(expected)


def test_partial_correlation_collapses_on_a_common_cause() -> None:
    z = [float(i) for i in range(10)]
    x = [v * 2 for v in z]
    y = [v * 3 for v in z]
    assert abs(correlation(x, y)) > 0.99
    assert abs(partial_correlation(x, y, z)) < 0.25


def test_inference_is_audited_and_persisted(tmp_path: Path) -> None:
    eng = _engine(tmp_path)
    for t in range(10):
        eng.observe(Observation(values={"a": float(t), "b": float(t) * 3}))
    eng.infer("a", "b")
    assert (tmp_path / "causal_graph.jsonl").read_text(encoding="utf-8").strip()
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "D-06" and e["action"] == "causal.infer" for e in entries)


def test_infer_all_covers_every_ordered_pair(tmp_path: Path) -> None:
    eng = _engine(tmp_path)
    for t in range(6):
        eng.observe(Observation(values={"a": float(t), "b": float(t) * 2, "c": 1.0}))
    links = eng.infer_all()
    assert len(links) == 3 * 2          # ordered pairs of 3 variables
