"""D-01 discriminating gate — recursive self-improvement proposal engine."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from aether.agents.introspection.failure_analysis import FailureAnalyser, FailureCluster
from aether.agents.safety.offline_review import ActionRejected, OfflineReviewQueue
from aether.agents.self_improvement.proposal_engine import (
    ProposalEngine,
    ProposalError,
    ProposalTracker,
)
from aether.kernel.audit.audit_log import AuditLog, AuditRecord
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _body(risk: int = 2, **over: Any) -> dict[str, Any]:
    body: dict[str, Any] = {
        "target_file": "aether/ai_core/model_manager/manager.py",
        "current_behaviour": "inference times out under load",
        "proposed_change": "add a retry with backoff",
        "expected_improvement": "fewer timeout failures",
        "test_to_verify": "pytest aether/tests/test_model_manager.py",
        "risk_level": risk,
    }
    body.update(over)
    return body


def _engine(
    tmp_path: Path,
    risk: int = 2,
    granted: bool = True,
    with_queue: bool = True,
    generate_fn=None,
) -> tuple[ProposalEngine, OfflineReviewQueue | None, AuditLog]:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("D-01", "proposal_engine.py", "CAP_SELF_IMPROVEMENT_PROPOSE")
    lb.register("C-08", "offline_review.py", "CAP_SAFETY_REVIEW")
    queue = (
        OfflineReviewQueue(
            lockbox=lb, store_path=tmp_path / "review_queue.jsonl", audit=audit
        )
        if with_queue
        else None
    )
    engine = ProposalEngine(
        lockbox=lb,
        generate_fn=generate_fn or (lambda _c: _body(risk)),
        analyser=FailureAnalyser(audit),
        tracker=ProposalTracker(tmp_path / "proposals.jsonl"),
        review_queue=queue,
        audit=audit,
    )
    return engine, queue, audit


def _inject_failures(audit: AuditLog, count: int = 3) -> None:
    for i in range(count):
        audit.append(
            AuditRecord(
                ddr="B-09",
                file="aether/ai_core/model_manager/manager.py",
                action="model.infer",
                gate_status="error",
                extra={"error": f"timeout after {30 + i}s"},
            )
        )


def test_proposal_from_failures(tmp_path: Path) -> None:
    """The mandated gate: 3 identical failures -> one proposal, right target."""
    engine, _q, audit = _engine(tmp_path)
    _inject_failures(audit, 3)

    clusters = engine.analyze_failures()
    assert len(clusters) == 1
    assert clusters[0].count == 3
    assert clusters[0].ddr == "B-09"

    proposal = engine.generate_proposal(clusters[0])
    assert proposal.target_file == "aether/ai_core/model_manager/manager.py"
    assert proposal.risk_level == 2
    assert proposal.evidence_count == 3
    assert proposal.source_ddr == "B-09"


def test_high_risk_cannot_be_applied_without_approval(tmp_path: Path) -> None:
    """S10 — the discriminator.

    Enforcing only at submit() would be theatre: the review queue fills up while
    changes ship anyway. The binding check must sit at the APPLICATION point.
    """
    engine, queue, audit = _engine(tmp_path, risk=4)
    _inject_failures(audit, 3)
    proposal = engine.submit(engine.generate_proposal(engine.analyze_failures()[0]))
    assert proposal.status == "under_review"
    assert proposal.review_req_id

    with pytest.raises(ActionRejected, match="not approved"):
        engine.mark_applied(proposal.prop_id, applied_by="prady", commit_sha="abc123")
    assert engine.tracker.get(proposal.prop_id).status == "under_review"  # type: ignore[union-attr]

    assert queue is not None
    queue.approve(proposal.review_req_id, approver="prady")
    applied = engine.mark_applied(
        proposal.prop_id, applied_by="prady", commit_sha="abc123"
    )
    assert applied.status == "applied"


def test_rejected_review_blocks_application(tmp_path: Path) -> None:
    engine, queue, audit = _engine(tmp_path, risk=5)
    _inject_failures(audit, 3)
    proposal = engine.submit(engine.generate_proposal(engine.analyze_failures()[0]))
    assert queue is not None
    queue.reject(proposal.review_req_id, approver="prady", reason="too invasive")
    with pytest.raises(ActionRejected, match="rejected"):
        engine.mark_applied(proposal.prop_id, applied_by="prady", commit_sha="abc")


def test_low_risk_applies_without_review(tmp_path: Path) -> None:
    engine, _q, audit = _engine(tmp_path, risk=1)
    _inject_failures(audit, 3)
    proposal = engine.submit(engine.generate_proposal(engine.analyze_failures()[0]))
    assert proposal.status == "proposed"
    assert proposal.review_req_id == ""
    applied = engine.mark_applied(proposal.prop_id, applied_by="prady", commit_sha="def")
    assert applied.status == "applied"


def test_high_risk_without_a_queue_is_refused(tmp_path: Path) -> None:
    """No queue must mean no submission, never a silent bypass."""
    engine, _q, audit = _engine(tmp_path, risk=4, with_queue=False)
    _inject_failures(audit, 3)
    proposal = engine.generate_proposal(engine.analyze_failures()[0])
    with pytest.raises(ProposalError, match="needs a review queue"):
        engine.submit(proposal)


def test_proposal_must_be_testable(tmp_path: Path) -> None:
    """An untestable self-change is exactly what must not be applied."""
    engine, _q, audit = _engine(
        tmp_path, generate_fn=lambda _c: _body(test_to_verify="   ")
    )
    _inject_failures(audit, 3)
    with pytest.raises(ProposalError, match="test_to_verify"):
        engine.generate_proposal(engine.analyze_failures()[0])


def test_missing_fields_refused(tmp_path: Path) -> None:
    engine, _q, audit = _engine(
        tmp_path, generate_fn=lambda _c: {"target_file": "x.py"}
    )
    _inject_failures(audit, 3)
    with pytest.raises(ProposalError, match="missing fields"):
        engine.generate_proposal(engine.analyze_failures()[0])


@pytest.mark.parametrize("risk", [0, 6, -1])
def test_risk_range_enforced(tmp_path: Path, risk: int) -> None:
    engine, _q, audit = _engine(tmp_path, generate_fn=lambda _c: _body(risk_level=risk))
    _inject_failures(audit, 3)
    with pytest.raises(ProposalError, match="out of range"):
        engine.generate_proposal(engine.analyze_failures()[0])


def test_risk_must_not_be_a_bool(tmp_path: Path) -> None:
    """True == 1 would sail through a naive range check as 'low risk'."""
    engine, _q, audit = _engine(tmp_path, generate_fn=lambda _c: _body(risk_level=True))
    _inject_failures(audit, 3)
    with pytest.raises(ProposalError, match="must be an int"):
        engine.generate_proposal(engine.analyze_failures()[0])


def test_isolated_failures_do_not_produce_proposals(tmp_path: Path) -> None:
    """One-off noise is not a pattern worth changing code for."""
    engine, _q, audit = _engine(tmp_path)
    _inject_failures(audit, 2)
    assert engine.analyze_failures() == []


def test_generation_failure_is_wrapped(tmp_path: Path) -> None:
    def explode(_c: FailureCluster) -> dict[str, Any]:
        raise RuntimeError("model offline")

    engine, _q, audit = _engine(tmp_path, generate_fn=explode)
    _inject_failures(audit, 3)
    with pytest.raises(ProposalError, match="generation failed"):
        engine.generate_proposal(engine.analyze_failures()[0])


def test_capability_required(tmp_path: Path) -> None:
    engine, _q, _a = _engine(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_SELF_IMPROVEMENT_PROPOSE"):
        engine.analyze_failures()


def test_double_apply_refused(tmp_path: Path) -> None:
    engine, _q, audit = _engine(tmp_path, risk=1)
    _inject_failures(audit, 3)
    proposal = engine.submit(engine.generate_proposal(engine.analyze_failures()[0]))
    engine.mark_applied(proposal.prop_id, applied_by="prady", commit_sha="a")
    with pytest.raises(ProposalError, match="already applied"):
        engine.mark_applied(proposal.prop_id, applied_by="prady", commit_sha="b")


def test_tracker_survives_restart(tmp_path: Path) -> None:
    engine, _q, audit = _engine(tmp_path, risk=1)
    _inject_failures(audit, 3)
    proposal = engine.submit(engine.generate_proposal(engine.analyze_failures()[0]))

    reborn = ProposalTracker(tmp_path / "proposals.jsonl")
    restored = reborn.get(proposal.prop_id)
    assert restored is not None
    assert restored.target_file == proposal.target_file


def test_end_to_end_scheduled_pass(tmp_path: Path) -> None:
    engine, _q, audit = _engine(tmp_path, risk=2)
    _inject_failures(audit, 4)
    proposals = engine.analyze_and_propose()
    assert len(proposals) == 1
    assert proposals[0].evidence_count == 4
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "D-01" and e["action"] == "proposal.submit" for e in entries)
