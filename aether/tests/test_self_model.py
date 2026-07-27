"""B-15 discriminating gate — self-model maintenance."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.agents.self_model.self_model import (
    MIN_OBSERVATIONS,
    SelfModel,
    UnevidencedClaim,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox


def _model(tmp_path: Path) -> SelfModel:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    lb.register("C-01", "knowledge_sync.py", "CAP_NETWORK_SYNC")
    return SelfModel(
        lockbox=lb, store_path=tmp_path / "self_model.jsonl", audit=audit
    )


def test_unevidenced_claim_refused(tmp_path: Path) -> None:
    """The discriminator.

    An unevidenced self-model is worse than none: C-05, D-01 and D-02 all treat
    it as ground truth when deciding what to attempt. A capability nobody
    granted must not be claimable.
    """
    sm = _model(tmp_path)
    with pytest.raises(UnevidencedClaim, match="does not hold"):
        sm.claim_capability("world domination", "C-01", "CAP_WATCHDOG")
    assert sm.claims == ()


def test_evidenced_claim_accepted(tmp_path: Path) -> None:
    sm = _model(tmp_path)
    claim = sm.claim_capability("sync knowledge", "C-01", "CAP_NETWORK_SYNC")
    assert claim.name == "sync knowledge"
    assert claim.confidence == 0.5              # unknown until observed


def test_confidence_is_derived_not_assigned(tmp_path: Path) -> None:
    sm = _model(tmp_path)
    sm.claim_capability("sync", "C-01", "CAP_NETWORK_SYNC")
    for _ in range(3):
        sm.record_outcome("sync", success=True)
    sm.record_outcome("sync", success=False)
    assert sm.confidence("sync") == pytest.approx(0.75)
    # There is no setter for confidence.
    claim = sm.claims[0]
    with pytest.raises(AttributeError):
        claim.confidence = 1.0  # type: ignore[misc]


def test_can_requires_an_established_record(tmp_path: Path) -> None:
    """An unobserved claim sits at 0.5 and must not read as a yes."""
    sm = _model(tmp_path)
    sm.claim_capability("sync", "C-01", "CAP_NETWORK_SYNC")
    assert sm.can("sync") is False               # 0.5 but unobserved
    for _ in range(MIN_OBSERVATIONS):
        sm.record_outcome("sync", success=True)
    assert sm.can("sync") is True
    assert sm.can("never-claimed") is False


def test_limit_requires_evidence(tmp_path: Path) -> None:
    sm = _model(tmp_path)
    with pytest.raises(ValueError, match="cite evidence"):
        sm.record_limit("cannot do maths", evidence="")
    sm.record_limit("cannot parse PDFs", evidence="audit:B-14 3 failures")
    assert len(sm.limits) == 1


def test_unknown_claim_outcome_raises(tmp_path: Path) -> None:
    sm = _model(tmp_path)
    with pytest.raises(KeyError, match="unknown capability claim"):
        sm.record_outcome("nope", success=True)


def test_state_survives_restart(tmp_path: Path) -> None:
    sm = _model(tmp_path)
    sm.claim_capability("sync", "C-01", "CAP_NETWORK_SYNC")
    sm.record_outcome("sync", success=True)
    sm.record_limit("no PDFs", evidence="audit")

    reborn = _model(tmp_path)
    assert reborn.confidence("sync") == pytest.approx(1.0)
    assert len(reborn.limits) == 1


def test_refused_claims_are_audited(tmp_path: Path) -> None:
    audit_path = tmp_path / "audit_log.jsonl"
    audit = AuditLog(audit_path)
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    sm = SelfModel(lockbox=lb, store_path=tmp_path / "sm.jsonl", audit=audit)
    with pytest.raises(UnevidencedClaim):
        sm.claim_capability("x", "NOBODY", "CAP_WATCHDOG")
    entries = AuditLog(audit_path).read_all()
    assert any(e["action"] == "self_model.unevidenced_claim" for e in entries)


def test_summary_shape(tmp_path: Path) -> None:
    sm = _model(tmp_path)
    sm.claim_capability("sync", "C-01", "CAP_NETWORK_SYNC")
    summary = sm.summary()
    assert "claims" in summary and "limits" in summary and "established" in summary
