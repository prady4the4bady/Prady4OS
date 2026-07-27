"""C-02 discriminating gate — consensus-based distributed experiments."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.agents.federation.distributed_experiments import (
    ExperimentError,
    ExperimentNode,
    quorum_for,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _node(tmp_path: Path, name: str, peers: list[str], granted: bool = True) -> ExperimentNode:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / f"{name}_tokens.jsonl", audit=audit)
    if granted:
        lb.register("C-02", "distributed_experiments.py", "CAP_EXPERIMENT_WRITE")
    return ExperimentNode(
        node_id=name, lockbox=lb, peers=peers,
        log_path=tmp_path / f"{name}_log.jsonl",
        results_path=tmp_path / f"{name}_results.jsonl",
        audit=audit,
    )


def test_quorum_vote(tmp_path: Path) -> None:
    """The mandated gate: 3 nodes, 2 for variant-0, 1 for variant-1 -> variant-0."""
    node = _node(tmp_path, "n1", ["n2", "n3"])
    node.propose("exp-1", "does prompt A beat prompt B?", ["variant-0", "variant-1"])
    node.vote("exp-1", 0, voter_id="n1")
    node.vote("exp-1", 0, voter_id="n2")
    node.vote("exp-1", 1, voter_id="n3")

    result = node.tally("exp-1")
    assert result.winning_variant == "variant-0"
    assert result.winning_index == 0
    assert result.tally == {0: 2, 1: 1}
    assert result.reason == "majority"


def test_quorum_counts_registered_peers_not_votes_cast(tmp_path: Path) -> None:
    """The discriminator.

    Quorum must be a majority of the MESH, not of whoever bothered to vote.
    Counting only cast votes lets two nodes in a twenty-node mesh decide policy
    — and every other test in this file still passes under that bug.
    """
    node = _node(tmp_path, "n1", [f"n{i}" for i in range(2, 21)])   # 20 peers
    assert node.quorum == 11
    node.propose("exp-1", "h", ["a", "b"])
    node.vote("exp-1", 0, voter_id="n1")
    node.vote("exp-1", 0, voter_id="n2")

    result = node.tally("exp-1")
    assert result.winning_variant is None
    assert "no quorum" in result.reason
    assert result.votes_cast == 2


def test_one_vote_per_node(tmp_path: Path) -> None:
    """Repeated voting must not stuff the ballot."""
    node = _node(tmp_path, "n1", ["n2", "n3"])
    node.propose("exp-1", "h", ["a", "b"])
    for _ in range(5):
        node.vote("exp-1", 0, voter_id="n2")      # same node, five times
    node.vote("exp-1", 1, voter_id="n1")
    node.vote("exp-1", 1, voter_id="n3")

    result = node.tally("exp-1")
    assert result.tally == {0: 1, 1: 2}
    assert result.winning_index == 1


def test_last_vote_wins_for_a_node(tmp_path: Path) -> None:
    node = _node(tmp_path, "n1", ["n2", "n3"])
    node.propose("exp-1", "h", ["a", "b"])
    node.vote("exp-1", 0, voter_id="n2")
    node.vote("exp-1", 1, voter_id="n2")          # changed its mind
    assert node.tally("exp-1").tally == {1: 1}


def test_tie_returns_none(tmp_path: Path) -> None:
    """Index order is proposer-controlled, so breaking ties by index is gameable."""
    node = _node(tmp_path, "n1", ["n2"])
    node.propose("exp-1", "h", ["a", "b"])
    node.vote("exp-1", 0, voter_id="n1")
    node.vote("exp-1", 1, voter_id="n2")
    result = node.tally("exp-1")
    assert result.winning_variant is None
    assert "tie" in result.reason


def test_late_vote_refused(tmp_path: Path) -> None:
    """A tally must not be overturnable after the deadline."""
    clock = {"t": 1000.0}
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "t.jsonl", audit=audit)
    lb.register("C-02", "f.py", "CAP_EXPERIMENT_WRITE")
    node = ExperimentNode(
        node_id="n1", lockbox=lb, peers=["n2"],
        log_path=tmp_path / "l.jsonl", results_path=tmp_path / "r.jsonl",
        audit=audit, now_fn=lambda: clock["t"],
    )
    node.propose("exp-1", "h", ["a", "b"], window_s=60.0)
    node.vote("exp-1", 0, voter_id="n1")
    clock["t"] += 3600.0
    with pytest.raises(ExperimentError, match="voting closed"):
        node.vote("exp-1", 1, voter_id="n2")


def test_capability_required(tmp_path: Path) -> None:
    node = _node(tmp_path, "n1", ["n2"], granted=False)
    with pytest.raises(CapabilityError, match="CAP_EXPERIMENT_WRITE"):
        node.propose("exp-1", "h", ["a", "b"])


def test_proposal_validation(tmp_path: Path) -> None:
    node = _node(tmp_path, "n1", ["n2"])
    with pytest.raises(ExperimentError, match="at least two variants"):
        node.propose("e1", "h", ["only-one"])
    with pytest.raises(ExperimentError, match="must be distinct"):
        node.propose("e2", "h", ["same", "same"])
    with pytest.raises(ExperimentError, match="too many variants"):
        node.propose("e3", "h", [f"v{i}" for i in range(20)])
    node.propose("e4", "h", ["a", "b"])
    with pytest.raises(ExperimentError, match="already exists"):
        node.propose("e4", "h", ["a", "b"])


def test_vote_on_unknown_or_bad_index(tmp_path: Path) -> None:
    node = _node(tmp_path, "n1", ["n2"])
    with pytest.raises(ExperimentError, match="unknown experiment"):
        node.vote("nope", 0)
    node.propose("exp-1", "h", ["a", "b"])
    with pytest.raises(ExperimentError, match="out of range"):
        node.vote("exp-1", 7)


def test_state_survives_restart(tmp_path: Path) -> None:
    node = _node(tmp_path, "n1", ["n2", "n3"])
    node.propose("exp-1", "h", ["a", "b"])
    node.vote("exp-1", 0, voter_id="n1")
    node.vote("exp-1", 0, voter_id="n2")

    reborn = _node(tmp_path, "n1", ["n2", "n3"])
    assert reborn.get("exp-1") is not None
    assert reborn.tally("exp-1").winning_index == 0


def test_peer_proposal_is_adopted(tmp_path: Path) -> None:
    a = _node(tmp_path, "n1", ["n2"])
    b = _node(tmp_path, "n2", ["n1"])
    proposal = a.propose("exp-1", "h", ["a", "b"])
    b.receive_proposal(proposal)
    assert b.get("exp-1") is not None
    b.vote("exp-1", 0, voter_id="n2")
    assert b.votes_for("exp-1")[0].variant_index == 0


def test_results_are_persisted_and_audited(tmp_path: Path) -> None:
    node = _node(tmp_path, "n1", ["n2"])
    node.propose("exp-1", "h", ["a", "b"])
    node.vote("exp-1", 0, voter_id="n1")
    node.vote("exp-1", 0, voter_id="n2")
    node.tally("exp-1")
    assert node.results()[0]["winning_variant"] == "a"
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "C-02" and e["action"] == "experiment.tally" for e in entries)


@pytest.mark.parametrize(
    ("peers", "expected"), [(1, 1), (2, 2), (3, 2), (4, 3), (5, 3), (20, 11)]
)
def test_quorum_formula(peers: int, expected: int) -> None:
    assert quorum_for(peers) == expected


def test_quorum_rejects_empty_mesh() -> None:
    with pytest.raises(ValueError, match="peer_count"):
        quorum_for(0)
