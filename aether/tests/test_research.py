"""Genome lineage (3D #61) and the D-07 hypothesis TREE (3D #60), DDR-851/855.

The dead-end registry (#63) is NOT tested here: it is D-13's
`FailureMemoryRegistry`, covered by `test_failure_registry.py`. The divergence
score DDR-855 added to it is tested there, next to the registry it belongs to,
rather than in a second file that would drift from it.
"""
from __future__ import annotations

import pytest

from aether.agents.research import Genome, GenomeArchive, ResearchError
from aether.agents.research.hypothesis_generator import Hypothesis, HypothesisStatus
from aether.agents.research.hypothesis_tree import (
    SCHEMA_VERSION,
    HypothesisTree,
    TreeError,
)


def _hyp(hid: str, statement: str = "") -> Hypothesis:
    return Hypothesis(
        hyp_id=hid,
        statement=statement or f"statement for {hid}",
        falsification_condition=f"{hid} fails if metric X stays below 0.9",
        expected_evidence="metric X from the experiment harness",
        estimated_cost=1.0,
    )


# ==========================================================================
# #60 hypothesis tree — versioning, lineage, persistence over D-07's type
# ==========================================================================
def test_tree_stores_the_d07_hypothesis_not_a_rival_type() -> None:
    """The whole point of DDR-855's remediation.

    Two notions of "hypothesis" would let the weaker one win wherever it was
    imported, and the weaker one had no falsification condition at all.
    """
    t = HypothesisTree()
    node = t.add(_hyp("H1"))
    assert isinstance(node.hypothesis, Hypothesis)
    assert node.hypothesis.falsification_condition
    assert node.hypothesis.estimated_cost == 1.0


def test_superseding_keeps_the_old_node() -> None:
    """Editing in place erases the reasoning that led somewhere wrong — and the
    dead-end registry can only match what is still on the record."""
    t = HypothesisTree()
    t.add(_hyp("H1"))
    new = t.supersede("H1", _hyp("H1.v2", "refined statement"))
    assert new.version == 2 and new.parent == "H1"
    assert t.get("H1").hypothesis.statement == "statement for H1"
    assert t.lineage("H1.v2") == ["H1", "H1.v2"]


def test_supersede_cannot_reuse_the_id() -> None:
    t = HypothesisTree()
    t.add(_hyp("H1"))
    with pytest.raises(TreeError, match="own id"):
        t.supersede("H1", _hyp("H1"))


def test_in_place_add_is_impossible() -> None:
    t = HypothesisTree()
    t.add(_hyp("H1"))
    with pytest.raises(TreeError, match="already in the tree"):
        t.add(_hyp("H1"))


def test_falsified_hypotheses_are_retained() -> None:
    t = HypothesisTree()
    t.add(_hyp("H1"))
    t.resolve("H1", HypothesisStatus.FALSIFIED, "experiment 12: metric fell to 0.4")
    assert [n.hyp_id for n in t.falsified] == ["H1"]
    assert t.get("H1").evidence.startswith("experiment 12")


def test_resolution_requires_evidence() -> None:
    """An unevidenced verdict is an opinion in the shape of a result."""
    t = HypothesisTree()
    t.add(_hyp("H1"))
    with pytest.raises(TreeError, match="requires evidence"):
        t.resolve("H1", HypothesisStatus.CONFIRMED, "   ")


def test_a_resolved_hypothesis_cannot_be_re_resolved() -> None:
    t = HypothesisTree()
    t.add(_hyp("H1"))
    t.resolve("H1", HypothesisStatus.FALSIFIED, "evidence A")
    with pytest.raises(TreeError, match="already falsified"):
        t.resolve("H1", HypothesisStatus.CONFIRMED, "evidence B")


def test_resolve_rejects_a_non_terminal_status() -> None:
    t = HypothesisTree()
    t.add(_hyp("H1"))
    with pytest.raises(TreeError, match="terminal status"):
        t.resolve("H1", HypothesisStatus.QUEUED, "e")


def test_missing_and_self_parent_are_refused() -> None:
    t = HypothesisTree()
    with pytest.raises(TreeError, match="not in the tree"):
        t.add(_hyp("H2"), parent="nope")
    with pytest.raises(TreeError, match="not in the tree"):
        t.add(_hyp("H1x"), parent="H1x")


def test_edges_point_strictly_backwards_so_lineage_terminates() -> None:
    """The acyclicity argument, asserted rather than assumed.

    A parent must exist before its child is added and no node is re-parented,
    so every edge points backwards in insertion order. That is why there is no
    cycle check to test — one would be unreachable, and a guard that never runs
    is a net nobody would notice breaking.
    """
    t = HypothesisTree()
    t.add(_hyp("H1"))
    t.add(_hyp("H2"), parent="H1")
    t.supersede("H2", _hyp("H2.v2"))
    order = t.to_dict()["nodes"]
    index = {n["hyp_id"]: i for i, n in enumerate(order)}
    for n in order:
        if n["parent"] is not None:
            assert index[n["parent"]] < index[n["hyp_id"]]
    assert t.lineage("H2.v2") == ["H1", "H2", "H2.v2"]


def test_children() -> None:
    t = HypothesisTree()
    t.add(_hyp("H1"))
    t.add(_hyp("H2"), parent="H1")
    t.add(_hyp("H3"), parent="H1")
    assert {c.hyp_id for c in t.children("H1")} == {"H2", "H3"}


def test_round_trips_for_persistence_across_boots() -> None:
    t = HypothesisTree()
    t.add(_hyp("H1"))
    t.supersede("H1", _hyp("H1.v2", "refined"))
    t.resolve("H1", HypothesisStatus.FALSIFIED, "experiment 12")
    back = HypothesisTree.from_dict(t.to_dict())
    assert back.to_dict() == t.to_dict()
    assert back.get("H1").hypothesis.status is HypothesisStatus.FALSIFIED
    assert back.get("H1").hypothesis.falsification_condition


def test_unknown_schema_is_refused() -> None:
    """Refusing to guess at a format this build does not know."""
    with pytest.raises(TreeError, match="unknown hypothesis tree schema"):
        HypothesisTree.from_dict({"schema": SCHEMA_VERSION + 99, "nodes": []})


def test_get_unknown_raises() -> None:
    with pytest.raises(TreeError, match="no hypothesis"):
        HypothesisTree().get("nope")


# ==========================================================================
# #61 genome.md
# ==========================================================================
def test_seed_then_evolve_builds_a_lineage() -> None:
    a = GenomeArchive("LUMYN")
    a.seed({"search": "breadth", "depth": "3"})
    a.evolve({"search": "depth", "depth": "3"}, "breadth plateaued at gen 0")
    assert [g.generation for g in a.lineage] == [0, 1]
    assert a.current.parent_generation == 0
    assert a.at(0).traits["search"] == "breadth", "the old genome is retained"


def test_mutation_requires_a_rationale() -> None:
    """A change with no stated reason records THAT something changed, not why —
    and why is the part that says whether to do it again."""
    a = GenomeArchive("LUMYN")
    a.seed({"search": "breadth"})
    with pytest.raises(ResearchError, match="requires a rationale"):
        a.evolve({"search": "depth"}, "   ")


def test_empty_generation_is_refused() -> None:
    a = GenomeArchive("LUMYN")
    a.seed({"search": "breadth"})
    with pytest.raises(ResearchError, match="no trait change"):
        a.evolve({"search": "breadth"}, "no reason really")


def test_diff_reports_added_removed_and_changed() -> None:
    g0 = Genome("A", 0, {"x": "1", "y": "2"})
    g1 = Genome("A", 1, {"x": "9", "z": "3"}, 0, "why")
    assert g0.diff(g1) == {"x": ("1", "9"), "y": ("2", None), "z": (None, "3")}


def test_archive_must_be_seeded_before_evolving() -> None:
    with pytest.raises(ResearchError, match="must be seeded"):
        GenomeArchive("A").evolve({"x": "1"}, "why")
    with pytest.raises(ResearchError, match="empty"):
        GenomeArchive("A").current


def test_double_seed_is_refused() -> None:
    a = GenomeArchive("A")
    a.seed({"x": "1"})
    with pytest.raises(ResearchError, match="already seeded"):
        a.seed({"x": "2"})


def test_genome_validation() -> None:
    with pytest.raises(ResearchError, match="agent"):
        Genome("  ", 0, {"x": "1"})
    with pytest.raises(ResearchError, match="at least one trait"):
        Genome("A", 0, {})
    with pytest.raises(ResearchError, match="negative"):
        Genome("A", -1, {"x": "1"})
    with pytest.raises(ResearchError, match="no generation"):
        GenomeArchive("A").at(5)
