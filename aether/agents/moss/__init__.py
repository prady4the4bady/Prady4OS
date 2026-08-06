"""MOSS source-rewriting pipeline (Section 3D #55, DDR-851).

Spec: *"staging sandbox, regression gate, SFS snapshot rollback"*, and from §3C:
`ACTION_REWRITE_AGENT_CODE` is *force-PENDING* with `CAP_REWRITE` +
`CAP_SOVEREIGN` **co-approval**, and *"regression suite must pass first"*.

This is an agent modifying its own source. Every guard below exists because the
corresponding failure is unrecoverable-by-the-agent: once a rewrite has broken
the thing that would have caught the rewrite, nothing downstream helps.

    propose -> stage -> regress -> snapshot -> promote
                          |                       |
                          +--- fail -> discard    +--- fail -> rollback

**The rewrite is never applied in place.** It goes to a staging copy. In-place
edit means a failed regression run leaves broken source with nothing to restore
from, and the agent that would perform the restore is the one running the broken
source.

**A regression suite that did not RUN is not a pass.** This is the guard that
matters most and the easiest to get wrong: `if failures == 0` is true when zero
tests ran. The gate therefore requires a positive test count *and* zero
failures, and treats "no result" as a distinct state from "passed".

**Co-approval is two DIFFERENT principals.** One holding both capabilities is
not co-approval — it is one decision wearing two hats, which is precisely what a
two-key rule exists to prevent.

**Snapshot before promote, always.** A rollback path that was never created is
discovered at the moment it is needed.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

__all__ = [
    "MossError",
    "Stage",
    "RegressionResult",
    "Approval",
    "RewriteProposal",
    "MossPipeline",
    "CAP_REWRITE",
    "CAP_SOVEREIGN",
]

CAP_REWRITE = "CAP_REWRITE"
CAP_SOVEREIGN = "CAP_SOVEREIGN"


class MossError(Exception):
    """A rewrite that must not proceed."""


class Stage(Enum):
    PROPOSED = "proposed"
    STAGED = "staged"
    REGRESSED = "regressed"
    SNAPSHOTTED = "snapshotted"
    PROMOTED = "promoted"
    DISCARDED = "discarded"
    ROLLED_BACK = "rolled_back"


@dataclass(frozen=True)
class RegressionResult:
    """Outcome of the regression suite.

    `ran` is separate from `failures` deliberately. `failures == 0` is also true
    when the suite never executed, and that reading turns a broken harness into
    a green light — the single most dangerous confusion available here.
    """

    ran: bool
    total: int
    failures: int

    @property
    def passed(self) -> bool:
        return self.ran and self.total > 0 and self.failures == 0


@dataclass(frozen=True)
class Approval:
    """One principal's approval, with the capabilities they held."""

    principal: str
    caps: frozenset[str]


@dataclass
class RewriteProposal:
    """A proposed source rewrite. Force-PENDING: never auto-approved."""

    target_path: str
    original: str
    proposed: str
    author_agent: str
    stage: Stage = Stage.PROPOSED
    staged_path: str | None = None
    snapshot_id: str | None = None
    regression: RegressionResult | None = None
    approvals: list[Approval] = field(default_factory=list)

    def __post_init__(self) -> None:
        if not self.target_path.strip():
            raise MossError("rewrite must name a target path")
        if not self.author_agent.strip():
            raise MossError("rewrite must name its author — an unattributed "
                            "source change cannot be reviewed or reverted to a "
                            "decision")
        if self.proposed == self.original:
            raise MossError(
                f"{self.target_path}: proposed source is identical to the "
                "original. A no-op rewrite still consumes an approval and lands "
                "in the audit log as a change that was made"
            )
        if not self.proposed.strip():
            raise MossError(f"{self.target_path}: proposed source is empty")


class MossPipeline:
    """propose → stage → regress → snapshot → promote, with a rollback path."""

    def __init__(self, staging_root: str = "/staging", snapshot_prefix: str = "snap") -> None:
        self.staging_root = staging_root
        self.snapshot_prefix = snapshot_prefix
        self._snapshots: dict[str, str] = {}
        self._live: dict[str, str] = {}
        self._counter = 0

    # -- the live tree the pipeline promotes into --------------------------
    def install(self, path: str, source: str) -> None:
        """Seed the live tree (what a promote would replace)."""
        self._live[path] = source

    def read(self, path: str) -> str:
        return self._live[path]

    # -- stage --------------------------------------------------------------
    def stage(self, proposal: RewriteProposal) -> str:
        """Write the candidate to a staging path. Never touches the live tree.

        In-place edit means a failed regression leaves broken source with
        nothing to restore from — and the agent that would restore it is the one
        running the broken source.
        """
        if proposal.stage is not Stage.PROPOSED:
            raise MossError(f"cannot stage from {proposal.stage.value}")
        self._counter += 1
        proposal.staged_path = f"{self.staging_root}/{self._counter}/{proposal.target_path}"
        proposal.stage = Stage.STAGED
        return proposal.staged_path

    # -- regress ------------------------------------------------------------
    def regress(self, proposal: RewriteProposal, result: RegressionResult) -> bool:
        """Record the regression outcome. Discards the proposal on failure."""
        if proposal.stage is not Stage.STAGED:
            raise MossError(
                f"regression must run against a STAGED candidate, not "
                f"{proposal.stage.value} — running it against the live tree "
                "tests the code that is already there"
            )
        proposal.regression = result
        if not result.passed:
            proposal.stage = Stage.DISCARDED
            return False
        proposal.stage = Stage.REGRESSED
        return True

    # -- approvals ----------------------------------------------------------
    def approve(self, proposal: RewriteProposal, principal: str, caps: set[str]) -> None:
        """Record one principal's approval."""
        if not principal.strip():
            raise MossError("approval requires a named principal")
        if principal == proposal.author_agent:
            raise MossError(
                f"{principal} authored this rewrite and cannot approve it (S4). "
                "Self-approval of a source change is the failure the co-approval "
                "rule exists to prevent"
            )
        if any(a.principal == principal for a in proposal.approvals):
            raise MossError(
                f"{principal} has already approved — one principal approving "
                "twice is one decision counted twice, not co-approval"
            )
        proposal.approvals.append(Approval(principal, frozenset(caps)))

    def _check_co_approval(self, proposal: RewriteProposal) -> None:
        rewrite = [a for a in proposal.approvals if CAP_REWRITE in a.caps]
        sovereign = [a for a in proposal.approvals if CAP_SOVEREIGN in a.caps]
        if not rewrite or not sovereign:
            raise MossError(
                f"co-approval incomplete: CAP_REWRITE={'yes' if rewrite else 'NO'}, "
                f"CAP_SOVEREIGN={'yes' if sovereign else 'NO'}. Both are required "
                "(3C, ACTION_REWRITE_AGENT_CODE)"
            )
        principals = {a.principal for a in rewrite} | {a.principal for a in sovereign}
        if len(principals) < 2:
            raise MossError(
                "co-approval requires TWO DIFFERENT principals. One principal "
                "holding both capabilities is one decision wearing two hats, "
                "which is exactly what a two-key rule exists to prevent"
            )

    # -- snapshot + promote -------------------------------------------------
    def snapshot(self, proposal: RewriteProposal) -> str:
        """Capture the live source before promotion. Always, no exceptions.

        A rollback path that was never created is discovered at the moment it
        is needed, which is the moment it cannot be created.
        """
        if proposal.stage is not Stage.REGRESSED:
            raise MossError(f"cannot snapshot from {proposal.stage.value}")
        sid = f"{self.snapshot_prefix}-{self._counter}"
        self._snapshots[sid] = self._live.get(proposal.target_path, proposal.original)
        proposal.snapshot_id = sid
        proposal.stage = Stage.SNAPSHOTTED
        return sid

    def promote(self, proposal: RewriteProposal) -> None:
        """Apply the staged source to the live tree."""
        if proposal.stage is not Stage.SNAPSHOTTED:
            raise MossError(
                f"cannot promote from {proposal.stage.value} — the pipeline is "
                "ordered so that a promote without a snapshot behind it is "
                "structurally impossible, not merely discouraged"
            )
        if proposal.regression is None or not proposal.regression.passed:
            raise MossError("promote requires a passed regression suite")
        self._check_co_approval(proposal)
        self._live[proposal.target_path] = proposal.proposed
        proposal.stage = Stage.PROMOTED

    def rollback(self, proposal: RewriteProposal) -> None:
        """Restore the snapshot taken before promotion."""
        if proposal.snapshot_id is None:
            raise MossError(
                "no snapshot to roll back to. This is the state the snapshot "
                "step exists to make unreachable"
            )
        self._live[proposal.target_path] = self._snapshots[proposal.snapshot_id]
        proposal.stage = Stage.ROLLED_BACK
