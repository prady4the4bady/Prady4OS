"""D-05 discriminating gate — meta-learning controller."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.agents.invention.registry import InventionRegistry
from aether.agents.meta_learning.meta_controller import (
    DEMOTION_MIN_USES,
    MIN_USES_FOR_PROMOTION,
    MetaController,
    MetaLearningError,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _controller(
    tmp_path: Path, granted: bool = True, with_registry: bool = False, bump=None
) -> tuple[MetaController, InventionRegistry | None]:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("D-05", "meta_controller.py", "CAP_META_LEARN")
    lb.register("C-04", "registry.py", "CAP_INVENTION_WRITE")
    registry = (
        InventionRegistry(
            lockbox=lb, store_path=tmp_path / "inventions.jsonl", audit=audit
        )
        if with_registry
        else None
    )
    mc = MetaController(
        lockbox=lb, store_path=tmp_path / "strategies.jsonl", audit=audit,
        invention_registry=registry, ontology_bump_fn=bump,
    )
    return mc, registry


def test_promote_demote(tmp_path: Path) -> None:
    """The mandated gate: A (5S/1F) wins; B (1S/5F) is flagged for demotion."""
    mc, _r = _controller(tmp_path)
    mc.register_strategy("A", "code")
    mc.register_strategy("B", "code")
    for _ in range(5):
        mc.record_outcome("A", success=True)
    mc.record_outcome("A", success=False)
    mc.record_outcome("B", success=True)
    for _ in range(5):
        mc.record_outcome("B", success=False)

    best = mc.get_best_strategy("code")
    assert best is not None and best.strategy_id == "A"
    assert best.promotion_score == pytest.approx(5 / 6, abs=1e-3)

    # B is bad but has only 6 uses — below the demotion floor.
    b = mc.get("B")
    assert b is not None and b.promotion_score < 0.2
    assert mc.should_demote(b) is False
    for _ in range(6):
        mc.record_outcome("B", success=False)
    assert mc.should_demote(mc.get("B")) is True    # type: ignore[arg-type]
    assert mc.sweep()["demoted"] == ["B"]


def test_one_lucky_success_does_not_outrank_evidence(tmp_path: Path) -> None:
    """The discriminator.

    A strategy used once, successfully, scores a perfect 1.0. Ranking on score
    alone crowns it over a 40-use strategy at 90% and commits the system to a
    single lucky sample.
    """
    mc, _r = _controller(tmp_path)
    mc.register_strategy("proven", "code")
    mc.register_strategy("lucky", "code")
    for _ in range(36):
        mc.record_outcome("proven", success=True)
    for _ in range(4):
        mc.record_outcome("proven", success=False)
    mc.record_outcome("lucky", success=True)          # 1 use, score 1.0

    lucky = mc.get("lucky")
    assert lucky is not None and lucky.promotion_score == 1.0
    assert lucky.eligible is False

    best = mc.get_best_strategy("code")
    assert best is not None and best.strategy_id == "proven"


def test_ties_break_toward_more_evidence(tmp_path: Path) -> None:
    mc, _r = _controller(tmp_path)
    for name, uses in (("thin", 3), ("thick", 30)):
        mc.register_strategy(name, "code")
        for _ in range(uses):
            mc.record_outcome(name, success=True)
    best = mc.get_best_strategy("code")
    assert best is not None and best.strategy_id == "thick"


def test_promotion_requires_evidence(tmp_path: Path) -> None:
    mc, _r = _controller(tmp_path)
    mc.register_strategy("thin", "code")
    mc.record_outcome("thin", success=True)
    with pytest.raises(MetaLearningError, match="required before promotion"):
        mc.promote_strategy("thin")

    for _ in range(MIN_USES_FOR_PROMOTION):
        mc.record_outcome("thin", success=True)
    assert mc.promote_strategy("thin").eligible is True


def test_promotion_registers_an_invention(tmp_path: Path) -> None:
    """I-04 — a promoted strategy becomes a durable capability."""
    mc, registry = _controller(tmp_path, with_registry=True)
    mc.register_strategy("A", "code", prompt_template="think step by step")
    for _ in range(MIN_USES_FOR_PROMOTION):
        mc.record_outcome("A", success=True)
    mc.promote_strategy("A")
    assert registry is not None
    active = registry.list_active()
    assert len(active) == 1
    assert "A" in active[0].title
    assert active[0].capability_delta["uses"] == MIN_USES_FOR_PROMOTION


def test_promotion_bumps_the_ontology(tmp_path: Path) -> None:
    """I-09 — proficiency rises when a strategy proves itself."""
    bumps: list[tuple[str, float]] = []
    mc, _r = _controller(tmp_path, bump=lambda d, step: bumps.append((d, step)))
    mc.register_strategy("A", "code")
    for _ in range(MIN_USES_FOR_PROMOTION):
        mc.record_outcome("A", success=True)
    mc.promote_strategy("A")
    assert bumps == [("code", 0.05)]


def test_demotion_is_harder_than_promotion(tmp_path: Path) -> None:
    """Retiring on a short unlucky run discards hard-won knowledge."""
    mc, _r = _controller(tmp_path)
    mc.register_strategy("unlucky", "code")
    for _ in range(DEMOTION_MIN_USES):
        mc.record_outcome("unlucky", success=False)
    record = mc.get("unlucky")
    assert record is not None
    assert record.promotion_score == 0.0
    assert mc.should_demote(record) is False     # exactly at the floor: not yet
    mc.record_outcome("unlucky", success=False)
    assert mc.should_demote(mc.get("unlucky")) is True    # type: ignore[arg-type]


def test_demoted_strategy_is_not_selected(tmp_path: Path) -> None:
    mc, _r = _controller(tmp_path)
    mc.register_strategy("A", "code")
    for _ in range(5):
        mc.record_outcome("A", success=True)
    assert mc.get_best_strategy("code") is not None
    mc.demote_strategy("A", reason="superseded")
    assert mc.get_best_strategy("code") is None


def test_unknown_domain_returns_none(tmp_path: Path) -> None:
    mc, _r = _controller(tmp_path)
    assert mc.get_best_strategy("nothing-here") is None


def test_record_outcome_can_autoregister_with_a_domain(tmp_path: Path) -> None:
    mc, _r = _controller(tmp_path)
    mc.record_outcome("new", success=True, domain="code")
    assert mc.get("new") is not None
    with pytest.raises(MetaLearningError, match="no domain to register"):
        mc.record_outcome("other", success=True)


def test_registration_validation(tmp_path: Path) -> None:
    mc, _r = _controller(tmp_path)
    with pytest.raises(MetaLearningError, match="id and a domain"):
        mc.register_strategy("", "code")


def test_capability_required(tmp_path: Path) -> None:
    mc, _r = _controller(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_META_LEARN"):
        mc.register_strategy("A", "code")


def test_state_survives_restart(tmp_path: Path) -> None:
    mc, _r = _controller(tmp_path)
    mc.register_strategy("A", "code")
    for _ in range(4):
        mc.record_outcome("A", success=True, latency_ms=100.0)

    reborn, _r2 = _controller(tmp_path)
    record = reborn.get("A")
    assert record is not None
    assert record.uses == 4
    assert record.promotion_score == 1.0
    assert record.avg_latency_ms == pytest.approx(100.0)


def test_promotion_is_audited(tmp_path: Path) -> None:
    mc, _r = _controller(tmp_path)
    mc.register_strategy("A", "code")
    for _ in range(MIN_USES_FOR_PROMOTION):
        mc.record_outcome("A", success=True)
    mc.promote_strategy("A")
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "D-05" and e["action"] == "meta.promote" for e in entries)


def test_summary_shape(tmp_path: Path) -> None:
    mc, _r = _controller(tmp_path)
    mc.register_strategy("A", "code")
    mc.record_outcome("A", success=True)
    summary = mc.summary()
    assert summary["A"]["domain"] == "code"
    assert summary["A"]["eligible"] is False
