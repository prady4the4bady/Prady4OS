"""C-08 discriminating gate — offline safety review mode."""

from __future__ import annotations

import asyncio
from pathlib import Path

import pytest

from aether.agents.safety.offline_review import (
    ActionRejected,
    OfflineReviewQueue,
    ReviewError,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _queue(tmp_path: Path, granted: bool = True) -> OfflineReviewQueue:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("C-08", "offline_review.py", "CAP_SAFETY_REVIEW")
    return OfflineReviewQueue(
        lockbox=lb, store_path=tmp_path / "review_queue.jsonl", audit=audit
    )


def test_approve_unblocks_agent(tmp_path: Path) -> None:
    """The mandated gate: enqueue, approve, the waiting agent proceeds."""

    async def scenario() -> str:
        q = _queue(tmp_path)
        req = q.enqueue(
            trigger_source="C-06", proposed_action="delete /data",
            requester_id="agent-x", risk_score=0.9,
        )

        async def approver() -> None:
            await asyncio.sleep(0)               # let the waiter park
            q.approve(req.req_id, approver="operator", reason="checked by hand")

        waiter = asyncio.create_task(q.wait_for(req.req_id, timeout_s=5))
        await approver()
        decided = await waiter
        return decided.status

    assert asyncio.run(scenario()) == "approved"


def test_timeout_denies_by_default(tmp_path: Path) -> None:
    """The discriminator.

    If nobody answers, the action must NOT proceed. A queue that falls through
    when unattended is a delay, not a review — and every other test in this
    file passes under that bug.
    """

    async def scenario() -> None:
        q = _queue(tmp_path)
        req = q.enqueue("C-06", "rm -rf /", requester_id="agent-x", risk_score=1.0)
        await q.wait_for(req.req_id, timeout_s=0.05)

    with pytest.raises(ActionRejected, match="timed out"):
        asyncio.run(scenario())


def test_rejection_raises_for_the_waiter(tmp_path: Path) -> None:
    async def scenario() -> None:
        q = _queue(tmp_path)
        req = q.enqueue("C-07", "exfiltrate", requester_id="agent-x")
        q.reject(req.req_id, approver="operator", reason="obviously not")
        await q.wait_for(req.req_id, timeout_s=1)

    with pytest.raises(ActionRejected, match="obviously not"):
        asyncio.run(scenario())


def test_requester_cannot_approve_itself(tmp_path: Path) -> None:
    """S3 — self-approval satisfies the letter of review and defeats it."""
    q = _queue(tmp_path)
    req = q.enqueue("C-06", "delete everything", requester_id="agent-x")
    with pytest.raises(ReviewError, match="cannot decide its own request"):
        q.approve(req.req_id, approver="agent-x")
    assert q.get(req.req_id).status == "pending"        # type: ignore[union-attr]


def test_decisions_are_terminal(tmp_path: Path) -> None:
    """A rejection must not be quietly overturned by a later call."""
    q = _queue(tmp_path)
    req = q.enqueue("C-06", "risky", requester_id="agent-x")
    q.reject(req.req_id, approver="operator", reason="no")
    with pytest.raises(ReviewError, match="already rejected"):
        q.approve(req.req_id, approver="operator2")
    assert q.get(req.req_id).status == "rejected"       # type: ignore[union-attr]


def test_rejection_requires_a_reason(tmp_path: Path) -> None:
    q = _queue(tmp_path)
    req = q.enqueue("C-06", "risky", requester_id="agent-x")
    with pytest.raises(ReviewError, match="rejection requires a reason"):
        q.reject(req.req_id, approver="operator", reason="")


def test_pending_survives_restart(tmp_path: Path) -> None:
    """A vanished high-risk request is a decision nobody made."""
    q = _queue(tmp_path)
    req = q.enqueue("C-06", "risky", requester_id="agent-x", risk_score=0.8)

    reborn = _queue(tmp_path)
    restored = reborn.get(req.req_id)
    assert restored is not None
    assert restored.status == "pending"
    assert restored.proposed_action == "risky"


def test_pending_is_risk_ordered(tmp_path: Path) -> None:
    q = _queue(tmp_path)
    q.enqueue("C-06", "low", requester_id="a", risk_score=0.2)
    q.enqueue("C-06", "high", requester_id="a", risk_score=0.95)
    q.enqueue("C-06", "mid", requester_id="a", risk_score=0.5)
    assert [r.proposed_action for r in q.list_pending()] == ["high", "mid", "low"]


def test_already_decided_returns_without_waiting(tmp_path: Path) -> None:
    async def scenario() -> str:
        q = _queue(tmp_path)
        req = q.enqueue("C-06", "risky", requester_id="agent-x")
        q.approve(req.req_id, approver="operator")
        decided = await q.wait_for(req.req_id, timeout_s=0.01)
        return decided.decided_by

    assert asyncio.run(scenario()) == "operator"


def test_capability_required(tmp_path: Path) -> None:
    q = _queue(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_SAFETY_REVIEW"):
        q.enqueue("C-06", "risky", requester_id="agent-x")


def test_enqueue_validates(tmp_path: Path) -> None:
    q = _queue(tmp_path)
    with pytest.raises(ReviewError, match="an action and a requester"):
        q.enqueue("C-06", "", requester_id="agent-x")
    with pytest.raises(ReviewError, match="an action and a requester"):
        q.enqueue("C-06", "risky", requester_id="")


def test_unknown_request(tmp_path: Path) -> None:
    async def scenario() -> None:
        q = _queue(tmp_path)
        await q.wait_for("no-such-id")

    with pytest.raises(ReviewError, match="unknown review request"):
        asyncio.run(scenario())


def test_self_approval_and_decisions_are_audited(tmp_path: Path) -> None:
    q = _queue(tmp_path)
    req = q.enqueue("C-06", "risky", requester_id="agent-x")
    with pytest.raises(ReviewError):
        q.approve(req.req_id, approver="agent-x")
    q.approve(req.req_id, approver="operator")
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    actions = {e["action"] for e in entries}
    assert "review.self_approval_refused" in actions
    assert "review.approved" in actions
