"""C-02 — consensus-based distributed experiments.

Nodes propose experiments (A/B prompt variants and the like), vote, and the
winner is decided by majority with a quorum floor.

The rules that make a vote meaningful rather than decorative:

* **quorum = floor(N/2) + 1** of the *registered* peers. Counting only the votes
  actually cast lets two nodes in a twenty-node mesh decide policy for everyone.
* **one vote per node**, last write wins but never double-counts. Without this a
  node can simply vote repeatedly to force an outcome.
* **the deadline is enforced** — a vote arriving after it is refused, otherwise
  a tally can be overturned indefinitely after the fact.
* **no quorum means `None`**, not "the best of a bad lot". A tie likewise
  returns `None` rather than picking by index, because index order is proposer-
  controlled and would be trivially gameable.

Everything is append-only: proposals and votes to ``experiment_log.jsonl``,
outcomes to ``experiment_results.jsonl``.
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Callable

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "ExperimentProposal",
    "Vote",
    "ExperimentResult",
    "ExperimentNode",
    "ExperimentError",
    "quorum_for",
]

DDR_ID = "C-02"
_FILE = "aether/agents/federation/distributed_experiments.py"
CAPABILITY = "CAP_EXPERIMENT_WRITE"

_HERE = Path(__file__).resolve().parent
DEFAULT_LOG = _HERE / "experiment_log.jsonl"
DEFAULT_RESULTS = _HERE / "experiment_results.jsonl"

DEFAULT_VOTE_WINDOW_S = 300.0
MAX_VARIANTS = 8


class ExperimentError(RuntimeError):
    """Proposal or vote refused."""


def quorum_for(peer_count: int) -> int:
    """Majority of the registered peers: floor(N/2) + 1."""
    if peer_count < 1:
        raise ValueError("peer_count must be >= 1")
    return peer_count // 2 + 1


@dataclass(slots=True)
class ExperimentProposal:
    exp_id: str
    proposer_id: str
    hypothesis: str
    variants: tuple[str, ...]
    vote_deadline_ts: float
    ts: float = field(default_factory=time.time)

    def to_json(self) -> str:
        payload = asdict(self)
        payload["variants"] = list(self.variants)
        payload["kind"] = "proposal"
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))


@dataclass(slots=True)
class Vote:
    exp_id: str
    voter_id: str
    variant_index: int
    ts: float = field(default_factory=time.time)

    def to_json(self) -> str:
        payload = asdict(self)
        payload["kind"] = "vote"
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))


@dataclass(slots=True)
class ExperimentResult:
    exp_id: str
    winning_variant: str | None
    winning_index: int | None
    tally: dict[int, int]
    votes_cast: int
    quorum: int
    reason: str
    ts: float = field(default_factory=time.time)

    def to_json(self) -> str:
        payload = asdict(self)
        payload["tally"] = {str(k): v for k, v in self.tally.items()}
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))


class ExperimentNode:
    """One participant: proposes, votes, tallies."""

    def __init__(
        self,
        node_id: str,
        lockbox: Lockbox,
        peers: list[str] | None = None,
        log_path: str | Path | None = None,
        results_path: str | Path | None = None,
        audit: AuditLog | None = None,
        now_fn: Callable[[], float] | None = None,
    ) -> None:
        self.node_id = node_id
        self.lockbox = lockbox
        # The node itself counts as a peer for quorum purposes.
        self._peers: set[str] = {node_id} | set(peers or ())
        self.log_path = Path(log_path) if log_path is not None else DEFAULT_LOG
        self.results_path = (
            Path(results_path) if results_path is not None else DEFAULT_RESULTS
        )
        for path in (self.log_path, self.results_path):
            path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self.now_fn: Callable[[], float] = now_fn if now_fn is not None else time.time
        self._proposals: dict[str, ExperimentProposal] = {}
        self._votes: dict[str, dict[str, Vote]] = {}
        self._replay()

    # -- membership ----------------------------------------------------------
    def add_peer(self, peer_id: str) -> None:
        self._peers.add(peer_id)

    @property
    def peers(self) -> tuple[str, ...]:
        return tuple(sorted(self._peers))

    @property
    def quorum(self) -> int:
        return quorum_for(len(self._peers))

    # -- persistence ---------------------------------------------------------
    def _append(self, line: str) -> None:
        with self.log_path.open("a", encoding="utf-8") as fh:
            fh.write(line + "\n")

    def _replay(self) -> None:
        if not self.log_path.is_file():
            return
        with self.log_path.open("r", encoding="utf-8") as fh:
            for raw in fh:
                stripped = raw.strip()
                if not stripped:
                    continue
                try:
                    rec = json.loads(stripped)
                except json.JSONDecodeError as exc:
                    raise ExperimentError(
                        f"corrupt experiment log: {self.log_path}"
                    ) from exc
                if rec.get("kind") == "proposal":
                    prop = ExperimentProposal(
                        exp_id=rec["exp_id"], proposer_id=rec["proposer_id"],
                        hypothesis=rec["hypothesis"],
                        variants=tuple(rec["variants"]),
                        vote_deadline_ts=rec["vote_deadline_ts"],
                        ts=rec.get("ts", 0.0),
                    )
                    self._proposals[prop.exp_id] = prop
                elif rec.get("kind") == "vote":
                    vote = Vote(
                        exp_id=rec["exp_id"], voter_id=rec["voter_id"],
                        variant_index=int(rec["variant_index"]),
                        ts=rec.get("ts", 0.0),
                    )
                    self._votes.setdefault(vote.exp_id, {})[vote.voter_id] = vote

    # -- proposing -----------------------------------------------------------
    def propose(
        self,
        exp_id: str,
        hypothesis: str,
        variants: list[str],
        window_s: float = DEFAULT_VOTE_WINDOW_S,
    ) -> ExperimentProposal:
        self.lockbox.require(DDR_ID, CAPABILITY)
        if exp_id in self._proposals:
            raise ExperimentError(f"experiment already exists: {exp_id}")
        if len(variants) < 2:
            raise ExperimentError("an experiment needs at least two variants")
        if len(variants) > MAX_VARIANTS:
            raise ExperimentError(f"too many variants: {len(variants)} > {MAX_VARIANTS}")
        if len(set(variants)) != len(variants):
            raise ExperimentError("variants must be distinct")

        proposal = ExperimentProposal(
            exp_id=exp_id,
            proposer_id=self.node_id,
            hypothesis=hypothesis,
            variants=tuple(variants),
            vote_deadline_ts=self.now_fn() + float(window_s),
        )
        self._proposals[exp_id] = proposal
        self._append(proposal.to_json())
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="experiment.propose",
            gate_status="proposed", exp_id=exp_id, variants=len(variants),
        )
        return proposal

    def receive_proposal(self, proposal: ExperimentProposal) -> ExperimentProposal:
        """Accept a peer's proposal into the local view."""
        if proposal.exp_id not in self._proposals:
            self._proposals[proposal.exp_id] = proposal
            self._append(proposal.to_json())
        self.add_peer(proposal.proposer_id)
        return self._proposals[proposal.exp_id]

    def get(self, exp_id: str) -> ExperimentProposal | None:
        return self._proposals.get(exp_id)

    # -- voting --------------------------------------------------------------
    def vote(self, exp_id: str, variant_index: int, voter_id: str | None = None) -> Vote:
        self.lockbox.require(DDR_ID, CAPABILITY)
        proposal = self._proposals.get(exp_id)
        if proposal is None:
            raise ExperimentError(f"unknown experiment: {exp_id}")
        if not 0 <= variant_index < len(proposal.variants):
            raise ExperimentError(
                f"variant index {variant_index} out of range for {exp_id}"
            )
        if self.now_fn() > proposal.vote_deadline_ts:
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="experiment.vote_late",
                gate_status="rejected", exp_id=exp_id,
            )
            raise ExperimentError(f"voting closed for {exp_id}")

        who = voter_id or self.node_id
        self.add_peer(who)
        vote = Vote(exp_id=exp_id, voter_id=who, variant_index=variant_index)
        # One vote per node: replacing, never appending a second count.
        self._votes.setdefault(exp_id, {})[who] = vote
        self._append(vote.to_json())
        return vote

    def votes_for(self, exp_id: str) -> tuple[Vote, ...]:
        return tuple(self._votes.get(exp_id, {}).values())

    # -- tallying ------------------------------------------------------------
    def tally(self, exp_id: str) -> ExperimentResult:
        proposal = self._proposals.get(exp_id)
        if proposal is None:
            raise ExperimentError(f"unknown experiment: {exp_id}")

        votes = self._votes.get(exp_id, {})
        counts: dict[int, int] = {}
        for vote in votes.values():
            counts[vote.variant_index] = counts.get(vote.variant_index, 0) + 1

        quorum = self.quorum
        result: ExperimentResult
        if len(votes) < quorum:
            result = ExperimentResult(
                exp_id=exp_id, winning_variant=None, winning_index=None,
                tally=counts, votes_cast=len(votes), quorum=quorum,
                reason=f"no quorum ({len(votes)} of {quorum})",
            )
        else:
            best = max(counts.values())
            leaders = sorted(i for i, c in counts.items() if c == best)
            if len(leaders) > 1:
                result = ExperimentResult(
                    exp_id=exp_id, winning_variant=None, winning_index=None,
                    tally=counts, votes_cast=len(votes), quorum=quorum,
                    reason=f"tie between variants {leaders}",
                )
            else:
                idx = leaders[0]
                result = ExperimentResult(
                    exp_id=exp_id, winning_variant=proposal.variants[idx],
                    winning_index=idx, tally=counts, votes_cast=len(votes),
                    quorum=quorum, reason="majority",
                )

        with self.results_path.open("a", encoding="utf-8") as fh:
            fh.write(result.to_json() + "\n")
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="experiment.tally",
            gate_status="decided" if result.winning_variant else "undecided",
            exp_id=exp_id, reason=result.reason,
        )
        return result

    def results(self) -> list[dict[str, Any]]:
        if not self.results_path.is_file():
            return []
        out: list[dict[str, Any]] = []
        with self.results_path.open("r", encoding="utf-8") as fh:
            for line in fh:
                if line.strip():
                    out.append(json.loads(line))
        return out
