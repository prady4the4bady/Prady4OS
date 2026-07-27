"""C-08 — offline safety review mode.

When something high-risk is flagged — C-06 confidence below threshold, a
severity>=4 custom invariant, a crash-looping agent — execution stops here and
waits for a human.

The properties that make this a real gate rather than a speed bump:

* **Default deny on timeout.** If nobody answers, ``wait_for`` raises
  ``ActionRejected``. A queue that proceeds when unattended is not a review
  queue; it is a delay.
* **The requester cannot approve itself.** ``approve`` refuses when the approver
  matches the request's originating agent — otherwise **S3** is satisfiable by
  an agent simply calling approve on its own request.
* **Decisions are terminal.** An approved or rejected request cannot be flipped
  afterwards, so a rejection cannot be quietly overturned by a later call.
* **Nothing is lost on restart.** The queue is append-only and replays, because
  a pending high-risk action that vanishes on restart has effectively been
  denied without anyone deciding — or worse, retried.
"""

from __future__ import annotations

import asyncio
import json
import time
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Literal

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "ReviewRequest",
    "OfflineReviewQueue",
    "ActionRejected",
    "ReviewError",
    "ReviewStatus",
    "DEFAULT_WAIT_TIMEOUT_S",
]

DDR_ID = "C-08"
_FILE = "aether/agents/safety/offline_review.py"
CAPABILITY = "CAP_SAFETY_REVIEW"

DEFAULT_STORE = Path(__file__).resolve().parent / "review_queue.jsonl"
DEFAULT_WAIT_TIMEOUT_S = 300.0

ReviewStatus = Literal["pending", "approved", "rejected"]


class ReviewError(RuntimeError):
    """Queue operation refused."""


class ActionRejected(PermissionError):
    """The reviewed action must not proceed."""


@dataclass(slots=True)
class ReviewRequest:
    req_id: str
    trigger_source: str
    requester_id: str
    proposed_action: str
    context: dict[str, Any] = field(default_factory=dict)
    risk_score: float = 0.0
    status: ReviewStatus = "pending"
    decided_by: str = ""
    reason: str = ""
    ts: float = field(default_factory=time.time)

    def to_json(self) -> str:
        return json.dumps(asdict(self), sort_keys=True, separators=(",", ":"))


class OfflineReviewQueue:
    """Halts high-risk actions until a human decides."""

    def __init__(
        self,
        lockbox: Lockbox,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
    ) -> None:
        self.lockbox = lockbox
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self._requests: dict[str, ReviewRequest] = {}
        self._events: dict[str, asyncio.Event] = {}
        self._replay()

    def _replay(self) -> None:
        if not self.store_path.is_file():
            return
        with self.store_path.open("r", encoding="utf-8") as fh:
            for raw in fh:
                stripped = raw.strip()
                if not stripped:
                    continue
                try:
                    rec = json.loads(stripped)
                except json.JSONDecodeError as exc:
                    raise ReviewError(
                        f"corrupt review queue: {self.store_path}"
                    ) from exc
                self._requests[rec["req_id"]] = ReviewRequest(
                    req_id=rec["req_id"], trigger_source=rec["trigger_source"],
                    requester_id=rec.get("requester_id", ""),
                    proposed_action=rec["proposed_action"],
                    context=rec.get("context", {}),
                    risk_score=rec.get("risk_score", 0.0),
                    status=rec.get("status", "pending"),
                    decided_by=rec.get("decided_by", ""),
                    reason=rec.get("reason", ""),
                    ts=rec.get("ts", 0.0),
                )

    def _append(self, req: ReviewRequest) -> None:
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(req.to_json() + "\n")

    def _event(self, req_id: str) -> asyncio.Event:
        event = self._events.get(req_id)
        if event is None:
            event = asyncio.Event()
            self._events[req_id] = event
        return event

    # -- enqueue -------------------------------------------------------------
    def enqueue(
        self,
        trigger_source: str,
        proposed_action: str,
        requester_id: str,
        context: dict[str, Any] | None = None,
        risk_score: float = 0.0,
    ) -> ReviewRequest:
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not proposed_action or not requester_id:
            raise ReviewError("a review request needs an action and a requester")
        req = ReviewRequest(
            req_id=str(uuid.uuid4()),
            trigger_source=trigger_source,
            requester_id=requester_id,
            proposed_action=proposed_action,
            context=dict(context or {}),
            risk_score=float(risk_score),
        )
        self._requests[req.req_id] = req
        self._append(req)
        self._event(req.req_id)
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="review.enqueue", gate_status="pending",
            req_id=req.req_id, source=trigger_source, requester=requester_id,
            risk=req.risk_score,
        )
        return req

    # -- decisions -----------------------------------------------------------
    def _decide(
        self, req_id: str, status: ReviewStatus, approver: str, reason: str
    ) -> ReviewRequest:
        req = self._requests.get(req_id)
        if req is None:
            raise ReviewError(f"unknown review request: {req_id}")
        if req.status != "pending":
            raise ReviewError(
                f"{req_id} is already {req.status}; decisions are terminal"
            )
        if not approver:
            raise ReviewError("a decision needs an approver")
        # S3 — an agent approving its own request satisfies the letter of review
        # while defeating all of its purpose.
        if approver == req.requester_id:
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="review.self_approval_refused",
                gate_status="refused", req_id=req_id, approver=approver,
            )
            raise ReviewError(
                f"{approver} cannot decide its own request {req_id} (S3)"
            )
        req.status = status
        req.decided_by = approver
        req.reason = reason
        self._append(req)
        self._event(req_id).set()
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action=f"review.{status}", gate_status=status,
            req_id=req_id, approver=approver, reason=reason,
        )
        return req

    def approve(self, req_id: str, approver: str, reason: str = "") -> ReviewRequest:
        return self._decide(req_id, "approved", approver, reason)

    def reject(self, req_id: str, approver: str, reason: str) -> ReviewRequest:
        if not reason:
            raise ReviewError("rejection requires a reason")
        return self._decide(req_id, "rejected", approver, reason)

    # -- waiting -------------------------------------------------------------
    async def wait_for(
        self, req_id: str, timeout_s: float = DEFAULT_WAIT_TIMEOUT_S
    ) -> ReviewRequest:
        """Block until decided. Times out into REJECTION, never approval."""
        req = self._requests.get(req_id)
        if req is None:
            raise ReviewError(f"unknown review request: {req_id}")
        if req.status == "pending":
            try:
                await asyncio.wait_for(self._event(req_id).wait(), timeout=timeout_s)
            except (asyncio.TimeoutError, TimeoutError) as exc:
                self.audit.append_action(
                    ddr=DDR_ID, file=_FILE, action="review.timeout",
                    gate_status="rejected", req_id=req_id,
                )
                raise ActionRejected(
                    f"review of {req_id} timed out after {timeout_s:.0f}s; "
                    "denying by default"
                ) from exc
        req = self._requests[req_id]
        if req.status != "approved":
            raise ActionRejected(
                f"review of {req_id} was {req.status}: {req.reason or 'no reason given'}"
            )
        return req

    # -- reading -------------------------------------------------------------
    def get(self, req_id: str) -> ReviewRequest | None:
        return self._requests.get(req_id)

    def list_pending(self) -> list[ReviewRequest]:
        return sorted(
            (r for r in self._requests.values() if r.status == "pending"),
            key=lambda r: (-r.risk_score, r.ts),
        )
