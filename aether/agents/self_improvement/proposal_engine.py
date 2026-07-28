"""D-01 — recursive self-improvement proposal engine.

AETHER cannot retrain its own weights, but it can read its failure history and
propose precise, testable changes to its own code and configuration. This is the
architectural foundation of recursive self-improvement, and the reason it is
built as a *proposal* pipeline rather than an edit pipeline.

The invariant that shapes the whole file is **S10 — never promote an improvement
proposal without human review if risk_level >= 3.** So:

* ``submit`` routes anything at risk >= 3 to Offline Safety Review (C-08) and
  leaves it ``under_review``;
* ``mark_applied`` **refuses** a risk >= 3 proposal that has no approved review
  request. Enqueuing without enforcing at the application point would be
  security theatre: the queue would fill up while changes shipped anyway.

Failure clustering is delegated to B-16, so the engine proposes against failure
*shapes* rather than individual log lines.
"""

from __future__ import annotations

import json
import time
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Callable, Literal

from aether.agents.introspection.failure_analysis import FailureAnalyser, FailureCluster
from aether.agents.safety.offline_review import ActionRejected, OfflineReviewQueue
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "ImprovementProposal",
    "ProposalEngine",
    "ProposalTracker",
    "ProposalError",
    "ProposalStatus",
    "REVIEW_RISK_THRESHOLD",
    "ANALYSIS_INTERVAL_S",
]

DDR_ID = "D-01"
_FILE = "aether/agents/self_improvement/proposal_engine.py"
CAPABILITY = "CAP_SELF_IMPROVEMENT_PROPOSE"

DEFAULT_STORE = Path(__file__).resolve().parent / "proposals.jsonl"
REVIEW_RISK_THRESHOLD = 3
ANALYSIS_INTERVAL_S = 6 * 3600.0          # 6 hours

ProposalStatus = Literal["proposed", "under_review", "applied", "rejected"]


class ProposalError(RuntimeError):
    """Proposal refused."""


@dataclass(slots=True)
class ImprovementProposal:
    prop_id: str
    source_ddr: str
    target_file: str
    current_behaviour: str
    proposed_change: str
    expected_improvement: str
    test_to_verify: str
    risk_level: int
    status: ProposalStatus = "proposed"
    review_req_id: str = ""
    applied_by: str = ""
    commit_sha: str = ""
    evidence_count: int = 0
    ts: float = field(default_factory=time.time)

    @property
    def needs_review(self) -> bool:
        return self.risk_level >= REVIEW_RISK_THRESHOLD

    def to_json(self) -> str:
        return json.dumps(asdict(self), sort_keys=True, separators=(",", ":"))


#: Turns a failure cluster into a proposal body. Injected so gates are hermetic.
GenerateFn = Callable[[FailureCluster], dict[str, Any]]

_REQUIRED_FIELDS = (
    "target_file",
    "current_behaviour",
    "proposed_change",
    "expected_improvement",
    "test_to_verify",
    "risk_level",
)


class ProposalTracker:
    """Append-only proposal store with derived state."""

    def __init__(self, store_path: str | Path | None = None) -> None:
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self._index: dict[str, ImprovementProposal] = {}
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
                    raise ProposalError(
                        f"corrupt proposal store: {self.store_path}"
                    ) from exc
                self._index[rec["prop_id"]] = ImprovementProposal(**rec)

    def append(self, proposal: ImprovementProposal) -> ImprovementProposal:
        self._index[proposal.prop_id] = proposal
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(proposal.to_json() + "\n")
        return proposal

    def get(self, prop_id: str) -> ImprovementProposal | None:
        return self._index.get(prop_id)

    def list_pending(self) -> list[ImprovementProposal]:
        return sorted(
            (p for p in self._index.values() if p.status in ("proposed", "under_review")),
            key=lambda p: (-p.risk_level, p.ts),
        )

    def list_applied(self) -> list[ImprovementProposal]:
        return sorted(
            (p for p in self._index.values() if p.status == "applied"),
            key=lambda p: p.ts,
        )

    def all(self) -> list[ImprovementProposal]:
        return sorted(self._index.values(), key=lambda p: p.ts)


class ProposalEngine:
    """Reads failure history, proposes fixes, and gates their application."""

    def __init__(
        self,
        lockbox: Lockbox,
        generate_fn: GenerateFn,
        analyser: FailureAnalyser | None = None,
        tracker: ProposalTracker | None = None,
        review_queue: OfflineReviewQueue | None = None,
        audit: AuditLog | None = None,
        min_cluster_size: int = 3,
    ) -> None:
        self.lockbox = lockbox
        self.generate_fn = generate_fn
        self.audit = audit if audit is not None else AuditLog()
        self.analyser = analyser if analyser is not None else FailureAnalyser(self.audit)
        self.tracker = tracker if tracker is not None else ProposalTracker()
        self.review_queue = review_queue
        self.min_cluster_size = int(min_cluster_size)

    # -- analysis ------------------------------------------------------------
    def analyze_failures(self) -> list[FailureCluster]:
        """Recurring failure shapes worth proposing against (B-16 does the work)."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        clusters = [
            c for c in self.analyser.cluster() if c.count >= self.min_cluster_size
        ]
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="proposal.analyze", gate_status="ok",
            clusters=len(clusters),
        )
        return clusters

    def generate_proposal(self, cluster: FailureCluster) -> ImprovementProposal:
        """Turn one cluster into a validated proposal."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        try:
            body = self.generate_fn(cluster)
        except Exception as exc:
            raise ProposalError(
                f"generation failed: {type(exc).__name__}: {exc}"
            ) from exc
        if not isinstance(body, dict):
            raise ProposalError(
                f"generate_fn returned {type(body).__name__}, expected dict"
            )
        missing = [f for f in _REQUIRED_FIELDS if f not in body]
        if missing:
            raise ProposalError(f"proposal missing fields: {missing}")

        risk = body["risk_level"]
        if isinstance(risk, bool) or not isinstance(risk, int):
            raise ProposalError(f"risk_level must be an int, got {type(risk).__name__}")
        if not 1 <= risk <= 5:
            raise ProposalError(f"risk_level {risk} out of range 1..5")
        if not str(body["test_to_verify"]).strip():
            # A proposal with no way to check it is not testable, and an
            # untestable self-change is exactly what must not be applied.
            raise ProposalError("proposal must carry a test_to_verify")

        return ImprovementProposal(
            prop_id=str(uuid.uuid4()),
            source_ddr=cluster.ddr,
            target_file=str(body["target_file"]),
            current_behaviour=str(body["current_behaviour"]),
            proposed_change=str(body["proposed_change"]),
            expected_improvement=str(body["expected_improvement"]),
            test_to_verify=str(body["test_to_verify"]),
            risk_level=risk,
            evidence_count=cluster.count,
        )

    def analyze_and_propose(self) -> list[ImprovementProposal]:
        """The scheduled 6-hourly pass: cluster, propose, submit."""
        out: list[ImprovementProposal] = []
        for cluster in self.analyze_failures():
            out.append(self.submit(self.generate_proposal(cluster)))
        return out

    # -- submission ----------------------------------------------------------
    def submit(self, proposal: ImprovementProposal) -> ImprovementProposal:
        """Record a proposal; risk >= 3 routes to Offline Safety Review (S10)."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        if proposal.needs_review:
            if self.review_queue is None:
                raise ProposalError(
                    f"risk {proposal.risk_level} proposal needs a review queue (S10)"
                )
            request = self.review_queue.enqueue(
                trigger_source=DDR_ID,
                proposed_action=f"apply {proposal.target_file}: {proposal.proposed_change}",
                requester_id=DDR_ID,
                context={"prop_id": proposal.prop_id, "test": proposal.test_to_verify},
                risk_score=proposal.risk_level / 5.0,
            )
            proposal.review_req_id = request.req_id
            proposal.status = "under_review"
        self.tracker.append(proposal)
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="proposal.submit",
            gate_status=proposal.status, prop_id=proposal.prop_id,
            target=proposal.target_file, risk=proposal.risk_level,
        )
        return proposal

    # -- application ---------------------------------------------------------
    def mark_applied(
        self, prop_id: str, applied_by: str, commit_sha: str
    ) -> ImprovementProposal:
        """Record that a proposal shipped — refused without review when risk >= 3.

        Enforcing only at ``submit`` would be theatre: the queue would fill while
        changes shipped anyway. **S10** binds here, at the application point.
        """
        self.lockbox.require(DDR_ID, CAPABILITY)
        proposal = self.tracker.get(prop_id)
        if proposal is None:
            raise ProposalError(f"unknown proposal: {prop_id}")
        if proposal.status == "applied":
            raise ProposalError(f"{prop_id} is already applied")
        if not applied_by or not commit_sha:
            raise ProposalError("mark_applied requires applied_by and commit_sha")

        if proposal.needs_review:
            if self.review_queue is None or not proposal.review_req_id:
                raise ProposalError(
                    f"{prop_id} is risk {proposal.risk_level} and has no review (S10)"
                )
            request = self.review_queue.get(proposal.review_req_id)
            if request is None or request.status != "approved":
                state = request.status if request else "missing"
                self.audit.append_action(
                    ddr=DDR_ID, file=_FILE, action="proposal.apply_refused",
                    gate_status="refused", prop_id=prop_id, review=state,
                )
                raise ActionRejected(
                    f"{prop_id} cannot be applied: review is {state}, not approved (S10)"
                )

        proposal.status = "applied"
        proposal.applied_by = applied_by
        proposal.commit_sha = commit_sha
        self.tracker.append(proposal)
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="proposal.applied", gate_status="applied",
            prop_id=prop_id, applied_by=applied_by, commit=commit_sha,
        )
        return proposal

    def reject(self, prop_id: str, reason: str) -> ImprovementProposal:
        self.lockbox.require(DDR_ID, CAPABILITY)
        proposal = self.tracker.get(prop_id)
        if proposal is None:
            raise ProposalError(f"unknown proposal: {prop_id}")
        if not reason:
            raise ProposalError("rejection requires a reason")
        proposal.status = "rejected"
        self.tracker.append(proposal)
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="proposal.rejected", gate_status="rejected",
            prop_id=prop_id, reason=reason,
        )
        return proposal
