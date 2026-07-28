"""D-03 discriminating gate — multi-model ensemble router."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from aether.ai_core.ensemble.ensemble_router import (
    EnsembleRouter,
    ModelProfile,
    NoModelAvailable,
    RoutingError,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _router(tmp_path: Path, granted: bool = True, escalate_fn=None) -> EnsembleRouter:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("D-03", "ensemble_router.py", "CAP_MODEL_ROUTE")
    return EnsembleRouter(
        lockbox=lb, store_path=tmp_path / "model_profiles.jsonl",
        audit=audit, escalate_fn=escalate_fn,
    )


def test_routes_to_best_model(tmp_path: Path) -> None:
    """The mandated gate: two profiles, the higher-quality one is chosen."""
    r = _router(tmp_path)
    r.register_model(
        ModelProfile("weak", "ollama", domains=("code",), quality_score=0.4)
    )
    r.register_model(
        ModelProfile("strong", "ollama", domains=("code",), quality_score=0.9)
    )
    assert r.route("code").model_id == "strong"


def test_domain_coverage_is_a_gate_not_a_weight(tmp_path: Path) -> None:
    """The discriminator.

    'fast_useless' is faster, cheaper and higher-quality in the abstract, but
    does not cover the domain. A router that weights domain match instead of
    gating on it picks it — and serves a request with an unsuitable model.
    """
    r = _router(tmp_path)
    r.register_model(
        ModelProfile(
            "fast_useless", "ollama", domains=("poetry",),
            quality_score=1.0, avg_latency_ms=1.0, cost_per_1k_tokens=0.0,
        )
    )
    r.register_model(
        ModelProfile(
            "slow_correct", "ollama", domains=("code",),
            quality_score=0.6, avg_latency_ms=4000.0, cost_per_1k_tokens=9.0,
        )
    )
    assert r.route("code", latency_budget_ms=1000.0).model_id == "slow_correct"


def test_quality_floor_escalates_rather_than_degrading(tmp_path: Path) -> None:
    """A floor is a floor. Silently serving the best of a bad set is how a
    system starts producing confident nonsense."""
    escalations: list[tuple[str, dict[str, Any]]] = []
    r = _router(tmp_path, escalate_fn=lambda d, c: escalations.append((d, c)))
    r.register_model(
        ModelProfile("mediocre", "ollama", domains=("code",), quality_score=0.5)
    )
    with pytest.raises(NoModelAvailable, match="quality >= 0.9"):
        r.route("code", quality_floor=0.9)
    assert escalations and escalations[0][0] == "code"


def test_generalist_covers_any_domain(tmp_path: Path) -> None:
    r = _router(tmp_path)
    r.register_model(ModelProfile("generalist", "ollama", quality_score=0.7))
    assert r.route("anything-at-all").model_id == "generalist"


def test_unavailable_model_is_skipped(tmp_path: Path) -> None:
    r = _router(tmp_path)
    r.register_model(
        ModelProfile("down", "ollama", domains=("code",), quality_score=0.99)
    )
    r.register_model(
        ModelProfile("up", "ollama", domains=("code",), quality_score=0.5)
    )
    r.unregister("down")
    assert r.route("code").model_id == "up"


def test_latency_penalty_applies(tmp_path: Path) -> None:
    r = _router(tmp_path)
    r.register_model(
        ModelProfile("slow", "ollama", domains=("code",),
                     quality_score=0.9, avg_latency_ms=8000.0)
    )
    r.register_model(
        ModelProfile("quick", "ollama", domains=("code",),
                     quality_score=0.7, avg_latency_ms=100.0)
    )
    assert r.route("code", latency_budget_ms=500.0).model_id == "quick"
    # With a generous budget the stronger model wins again.
    assert r.route("code", latency_budget_ms=20000.0).model_id == "slow"


def test_degrading_model_stops_being_chosen(tmp_path: Path) -> None:
    """Observed behaviour must feed back without anyone editing a config."""
    r = _router(tmp_path)
    r.register_model(
        ModelProfile("a", "ollama", domains=("code",), quality_score=0.9)
    )
    r.register_model(
        ModelProfile("b", "ollama", domains=("code",), quality_score=0.75)
    )
    assert r.route("code").model_id == "a"

    for _ in range(8):
        r.update_stats("a", actual_latency_ms=200.0, quality_rating=0.1)
    assert r.route("code").model_id == "b"


def test_first_observation_replaces_the_prior(tmp_path: Path) -> None:
    r = _router(tmp_path)
    r.register_model(ModelProfile("a", "ollama", quality_score=0.5))
    updated = r.update_stats("a", actual_latency_ms=1234.0, quality_rating=0.8)
    assert updated.avg_latency_ms == pytest.approx(1234.0)
    assert updated.quality_score == pytest.approx(0.8)
    assert updated.observations == 1


def test_stats_validation(tmp_path: Path) -> None:
    r = _router(tmp_path)
    r.register_model(ModelProfile("a", "ollama"))
    with pytest.raises(RoutingError, match="out of range"):
        r.update_stats("a", 100.0, quality_rating=5.0)
    with pytest.raises(RoutingError, match="unknown model"):
        r.update_stats("ghost", 100.0, 0.5)


def test_profile_validation() -> None:
    with pytest.raises(RoutingError, match="quality_score"):
        ModelProfile("bad", "ollama", quality_score=2.0)


def test_route_input_validation(tmp_path: Path) -> None:
    r = _router(tmp_path)
    r.register_model(ModelProfile("a", "ollama"))
    with pytest.raises(RoutingError, match="task_domain"):
        r.route("")
    with pytest.raises(RoutingError, match="quality_floor"):
        r.route("code", quality_floor=1.5)
    with pytest.raises(RoutingError, match="latency_budget_ms"):
        r.score(r.models[0], latency_budget_ms=0)


def test_capability_required(tmp_path: Path) -> None:
    r = _router(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_MODEL_ROUTE"):
        r.register_model(ModelProfile("a", "ollama"))


def test_infer_routes_and_records(tmp_path: Path) -> None:
    r = _router(tmp_path)
    r.register_model(
        ModelProfile("a", "ollama", domains=("code",), quality_score=0.8)
    )
    text, profile = r.infer(
        "write a function", task_domain="code",
        runners={"a": lambda prompt, _p: f"answer to {prompt}"},
    )
    assert text == "answer to write a function"
    assert profile.model_id == "a"
    assert r.models[0].observations == 1


def test_infer_requires_a_runner(tmp_path: Path) -> None:
    r = _router(tmp_path)
    r.register_model(ModelProfile("a", "ollama", domains=("code",)))
    with pytest.raises(RoutingError, match="no runner registered"):
        r.infer("x", task_domain="code", runners={})


def test_profiles_survive_restart(tmp_path: Path) -> None:
    r = _router(tmp_path)
    r.register_model(
        ModelProfile("a", "ollama", domains=("code",), quality_score=0.8)
    )
    reborn = _router(tmp_path)
    assert reborn.route("code").model_id == "a"


def test_routing_is_audited(tmp_path: Path) -> None:
    r = _router(tmp_path)
    r.register_model(ModelProfile("a", "ollama", domains=("code",)))
    r.route("code")
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "D-03" and e["action"] == "route.select" for e in entries)
