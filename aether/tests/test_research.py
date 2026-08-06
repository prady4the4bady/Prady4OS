"""Hypothesis tree, genome lineage, dead-end registry (3D #60/#61/#63)."""
from __future__ import annotations

import pytest

from aether.agents.research import (
    MIN_DIVERGENCE,
    DeadEnd,
    DeadEndRegistry,
    Genome,
    GenomeArchive,
    Hypothesis,
    HypothesisStatus,
    HypothesisTree,
    ResearchError,
    divergence,
)


def _h(hid: str, **kw) -> Hypothesis:
    base = dict(hid=hid, statement=f"statement for {hid}",
                prediction=f"if {hid} holds, metric X rises above 0.9")
    base.update(kw)
    return Hypothesis(**base)


# ==========================================================================
# #60 hypothesis tree
# ==========================================================================
def test_hypothesis_requires_a_prediction() -> None:
    """One that cannot be wrong cannot be right either."""
    with pytest.raises(ResearchError, match="prediction"):
        Hypothesis("H1", "vague idea", "   ")


def test_superseding_keeps_the_old_version() -> None:
    """Editing in place erases the reasoning that led somewhere wrong."""
    t = HypothesisTree()
    t.add(_h("H1"))
    new = t.supersede("H1", "refined", "metric X rises above 0.95")
    assert new.hid == "H1.v2" and new.version == 2 and new.parent == "H1"
    assert t.get("H1").statement == "statement for H1"
    assert t.lineage("H1.v2") == ["H1", "H1.v2"]


def test_in_place_edit_is_impossible() -> None:
    t = HypothesisTree()
    t.add(_h("H1"))
    with pytest.raises(ResearchError, match="already exists"):
        t.add(_h("H1"))


def test_refuted_hypotheses_are_retained() -> None:
    """A research agent's failures are its most valuable output."""
    t = HypothesisTree()
    t.add(_h("H1"))
    t.resolve("H1", HypothesisStatus.REFUTED, "experiment 12: metric fell to 0.4")
    assert [h.hid for h in t.refuted] == ["H1"]
    assert t.get("H1").evidence.startswith("experiment 12")


def test_resolution_requires_evidence() -> None:
    """An unevidenced verdict is an opinion in the shape of a result."""
    t = HypothesisTree()
    t.add(_h("H1"))
    with pytest.raises(ResearchError, match="requires evidence"):
        t.resolve("H1", HypothesisStatus.SUPPORTED, "  ")


def test_a_resolved_hypothesis_cannot_be_re_resolved() -> None:
    """Re-resolving overwrites the first verdict and its evidence."""
    t = HypothesisTree()
    t.add(_h("H1"))
    t.resolve("H1", HypothesisStatus.REFUTED, "evidence A")
    with pytest.raises(ResearchError, match="already refuted"):
        t.resolve("H1", HypothesisStatus.SUPPORTED, "evidence B")


def test_resolve_rejects_a_non_terminal_status() -> None:
    t = HypothesisTree()
    t.add(_h("H1"))
    with pytest.raises(ResearchError, match="terminal status"):
        t.resolve("H1", HypothesisStatus.OPEN, "e")


def test_missing_parent_is_refused() -> None:
    t = HypothesisTree()
    with pytest.raises(ResearchError, match="does not exist"):
        t.add(_h("H2", parent="nope"))


def test_self_parent_is_refused() -> None:
    """Caught by parent-must-exist, which is also what makes cycles impossible."""
    with pytest.raises(ResearchError, match="does not exist"):
        HypothesisTree().add(_h("H1x", parent="H1x"))


def test_edges_point_strictly_backwards_so_lineage_terminates() -> None:
    """The acyclicity argument, asserted rather than assumed.

    A parent must exist before its child is added and no node is ever
    re-parented, so every edge points backwards in insertion order. That is why
    there is no cycle check to test — an explicit one would be unreachable, and
    a guard that never runs is a net nobody would notice breaking.
    """
    t = HypothesisTree()
    t.add(_h("H1"))
    t.add(_h("H2", parent="H1"))
    t.supersede("H2", "refined", "metric X rises above 0.99")
    order = t.to_dict()["nodes"]
    index = {n["hid"]: i for i, n in enumerate(order)}
    for n in order:
        if n["parent"] is not None:
            assert index[n["parent"]] < index[n["hid"]]
    assert t.lineage("H2.v2") == ["H1", "H2", "H2.v2"]


def test_children_and_lineage() -> None:
    t = HypothesisTree()
    t.add(_h("H1"))
    t.add(_h("H2", parent="H1"))
    t.add(_h("H3", parent="H1"))
    assert {c.hid for c in t.children("H1")} == {"H2", "H3"}
    assert t.lineage("H3") == ["H1", "H3"]


def test_round_trips_for_persistence_across_boots() -> None:
    t = HypothesisTree()
    t.add(_h("H1"))
    t.supersede("H1", "refined", "metric X rises above 0.95")
    t.resolve("H1", HypothesisStatus.REFUTED, "experiment 12")
    back = HypothesisTree.from_dict(t.to_dict())
    assert back.to_dict() == t.to_dict()
    assert back.get("H1").status is HypothesisStatus.REFUTED


def test_unknown_serialisation_version_is_refused() -> None:
    """Refusing to guess at a format this build does not know."""
    with pytest.raises(ResearchError, match="unknown hypothesis tree version"):
        HypothesisTree.from_dict({"version": 99, "nodes": []})


def test_get_unknown_raises() -> None:
    with pytest.raises(ResearchError, match="no hypothesis"):
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


# ==========================================================================
# #63 dead-end registry
# ==========================================================================
def test_dead_end_requires_a_failure_reason() -> None:
    """'It did not work' cannot be checked against a new attempt, so the
    registry grows while remaining unable to prevent anything."""
    with pytest.raises(ResearchError, match="failure REASON"):
        DeadEnd("tried gradient descent on the tokeniser", "   ")


def test_query_before_repeating_catches_a_near_duplicate() -> None:
    r = DeadEndRegistry()
    r.record(DeadEnd("tune the learning rate on the small corpus",
                     "diverged after 200 steps every seed", 5000))
    hit = r.check("tune the learning rate on the small corpus again")
    assert hit is not None
    assert "diverged" in hit.reason


def test_check_returns_the_entry_not_a_boolean() -> None:
    """A bare False makes the refusal unarguable — the operator needs to see
    what it collided with and why that failed."""
    r = DeadEndRegistry()
    r.record(DeadEnd("approach alpha beta gamma", "ran out of memory"))
    hit = r.check("approach alpha beta gamma")
    assert isinstance(hit, DeadEnd) and hit.reason == "ran out of memory"


def test_a_genuinely_different_approach_is_not_blocked() -> None:
    r = DeadEndRegistry()
    r.record(DeadEnd("tune the learning rate on the small corpus", "diverged"))
    assert r.check("rewrite the tokeniser to use byte pair encoding") is None


def test_divergence_bounds() -> None:
    assert divergence("a b c", "a b c") == 0.0
    assert divergence("a b c", "x y z") == 1.0
    assert 0.0 < divergence("a b c", "a b z") < 1.0
    assert divergence("", "") == 0.0
    assert divergence("a", "") == 1.0


def test_divergence_is_deterministic_and_explainable() -> None:
    """An embedding score would judge meaning better and make a refusal
    impossible to argue with."""
    assert divergence("one two", "two one") == divergence("two one", "one two")
    assert divergence("a b", "a c") == 0.6666666666666667 - 0.0 or True
    # exact value: |{a}| / |{a,b,c}| = 1/3 similarity -> 2/3 divergence
    assert abs(divergence("a b", "a c") - (2 / 3)) < 1e-9


def test_threshold_is_tunable_and_validated() -> None:
    r = DeadEndRegistry()
    r.record(DeadEnd("alpha beta gamma delta", "failed"))
    assert r.check("alpha beta gamma epsilon", threshold=0.1) is None
    assert r.check("alpha beta gamma epsilon", threshold=0.9) is not None
    with pytest.raises(ResearchError):
        r.check("x", threshold=1.5)
    with pytest.raises(ResearchError):
        r.check("   ")


def test_registry_is_bounded_and_counts_drops() -> None:
    r = DeadEndRegistry(capacity=3)
    for i in range(10):
        r.record(DeadEnd(f"approach number {i}", "failed"))
    assert len(r.entries) == 3 and r.dropped == 7
    assert r.entries[0].approach == "approach number 7", "oldest dropped"


def test_wasted_cost_is_the_case_for_querying() -> None:
    r = DeadEndRegistry()
    r.record(DeadEnd("a b c", "failed", 1000))
    r.record(DeadEnd("d e f", "failed", 2500))
    assert r.wasted_microcents() == 3500


def test_zero_capacity_is_refused() -> None:
    with pytest.raises(ResearchError):
        DeadEndRegistry(capacity=0)


def test_default_threshold_is_conservative() -> None:
    """A false 'too similar' costs one overridable refusal; a false
    'different enough' costs re-deriving a known failure."""
    assert 0.0 < MIN_DIVERGENCE < 0.5
