"""Vector KG, population tournament, run visualiser (3D #62/#64/#65, DDR-856)."""
from __future__ import annotations

import io

import pytest

from aether.agents.budget.trajectory import REDACTED, TrajectoryWriter
from aether.agents.memory.vector_graph import (
    VectorGraph,
    VectorGraphError,
    cosine,
)
from aether.agents.population import (
    MIN_MATCHES,
    Match,
    Tournament,
    TournamentError,
    Variant,
)
from aether.agents.visualiser import VisualiserError, render_html, render_summary


# ==========================================================================
# #62 vector knowledge graph
# ==========================================================================
def _g(dim: int = 3, **kw) -> VectorGraph:
    return VectorGraph(dim=dim, **kw)


def test_dimension_mismatch_is_refused_not_padded() -> None:
    """Padding silently changes direction, so the neighbour returned is
    confidently wrong rather than absent."""
    g = _g()
    with pytest.raises(VectorGraphError, match="Refusing to pad or truncate"):
        g.add("a", (1.0, 0.0))


def test_zero_vector_is_refused() -> None:
    g = _g()
    with pytest.raises(VectorGraphError, match="no direction"):
        g.add("a", (0.0, 0.0, 0.0))


def test_cosine_raises_on_zero_rather_than_returning_zero() -> None:
    """0.0 is a real similarity meaning orthogonal; using it for 'undefined'
    makes an error indistinguishable from a result."""
    with pytest.raises(VectorGraphError, match="undefined"):
        cosine((0.0, 0.0), (1.0, 0.0))


def test_cosine_basic_values() -> None:
    assert cosine((1.0, 0.0), (1.0, 0.0)) == pytest.approx(1.0)
    assert cosine((1.0, 0.0), (0.0, 1.0)) == pytest.approx(0.0)
    assert cosine((1.0, 0.0), (-1.0, 0.0)) == pytest.approx(-1.0)


def test_nan_and_infinity_are_refused() -> None:
    g = _g()
    with pytest.raises(VectorGraphError, match="NaN or infinity"):
        g.add("a", (float("nan"), 1.0, 1.0))
    with pytest.raises(VectorGraphError, match="NaN or infinity"):
        g.add("b", (float("inf"), 1.0, 1.0))


def test_full_graph_raises_rather_than_evicting() -> None:
    """The D-13 principle: silent forgetting turns a memory into something that
    cannot be reasoned about."""
    g = _g(capacity=2)
    g.add("a", (1.0, 0.0, 0.0))
    g.add("b", (0.0, 1.0, 0.0))
    with pytest.raises(VectorGraphError, match="Refusing to evict"):
        g.add("c", (0.0, 0.0, 1.0))
    assert len(g) == 2


def test_duplicate_add_is_refused_pointing_at_reinforce() -> None:
    g = _g()
    g.add("a", (1.0, 0.0, 0.0))
    with pytest.raises(VectorGraphError, match="reinforce"):
        g.add("a", (0.0, 1.0, 0.0))


def test_reinforce_moves_toward_new_evidence_without_reindex() -> None:
    """The online-learning property the spec names."""
    g = _g()
    g.add("a", (1.0, 0.0, 0.0))
    node = g.reinforce("a", (0.0, 1.0, 0.0), rate=0.5)
    assert node.vector == (0.5, 0.5, 0.0)
    assert node.updates == 1


def test_reinforce_rate_is_bounded() -> None:
    """0 is a silent no-op that looks like an update; >1 overshoots past the
    new evidence to somewhere neither observation supports."""
    g = _g()
    g.add("a", (1.0, 0.0, 0.0))
    for bad in (0.0, -0.5, 1.5):
        with pytest.raises(VectorGraphError, match="rate must be"):
            g.reinforce("a", (0.0, 1.0, 0.0), rate=bad)


def test_reinforcement_to_zero_is_refused() -> None:
    """Otherwise the node survives as something no query can ever match."""
    g = _g()
    g.add("a", (1.0, 1.0, 1.0))
    with pytest.raises(VectorGraphError, match="cancelled the embedding"):
        g.reinforce("a", (-1.0, -1.0, -1.0), rate=0.5)


def test_edge_needs_both_endpoints() -> None:
    g = _g()
    g.add("a", (1.0, 0.0, 0.0))
    with pytest.raises(VectorGraphError, match="dangling edge"):
        g.link("a", "ghost", "relates_to")


def test_edges_are_typed_and_deduplicated() -> None:
    g = _g()
    g.add("a", (1.0, 0.0, 0.0))
    g.add("b", (0.0, 1.0, 0.0))
    g.link("a", "b", "cites")
    g.link("a", "b", "contradicts")
    with pytest.raises(VectorGraphError, match="duplicate edge"):
        g.link("a", "b", "cites")
    assert {e.relation for e in g.neighbours("a")} == {"cites", "contradicts"}
    assert [e.relation for e in g.neighbours("a", relation="cites")] == ["cites"]
    with pytest.raises(VectorGraphError, match="requires a relation"):
        g.link("a", "b", "  ")


def test_nearest_ranks_by_similarity() -> None:
    g = _g()
    g.add("same", (1.0, 0.0, 0.0))
    g.add("orth", (0.0, 1.0, 0.0))
    g.add("oppo", (-1.0, 0.0, 0.0))
    ranked = g.nearest((1.0, 0.0, 0.0), k=3)
    assert [n.key for n, _ in ranked] == ["same", "orth", "oppo"]


def test_nodes_returns_insertion_order_not_a_second_sort() -> None:
    """One mechanism for the ordering guarantee, not two.

    Sorting here as well would make `nearest`'s tie-break dead code no test
    could kill — which is exactly what mutation testing found before DDR-856.
    """
    g = _g()
    for key in ("z", "a", "m"):
        g.add(key, (1.0, 0.0, 0.0))
    assert [n.key for n in g.nodes] == ["z", "a", "m"]


def test_nearest_is_deterministic_on_ties() -> None:
    """A ranking that depends on insertion order makes a bad recall
    impossible to reproduce.

    Every candidate ties on similarity here, so ONLY the explicit key
    tie-break can produce a stable answer.
    """
    a, b = _g(), _g()
    for g, order in ((a, ("x", "y", "z")), (b, ("z", "y", "x"))):
        for key in order:
            g.add(key, (1.0, 0.0, 0.0))
    assert [n.key for n, _ in a.nearest((1.0, 0.0, 0.0))] == \
           [n.key for n, _ in b.nearest((1.0, 0.0, 0.0))]


def test_nearest_respects_k_and_threshold() -> None:
    g = _g()
    g.add("same", (1.0, 0.0, 0.0))
    g.add("oppo", (-1.0, 0.0, 0.0))
    assert len(g.nearest((1.0, 0.0, 0.0), k=1)) == 1
    assert [n.key for n, _ in g.nearest((1.0, 0.0, 0.0), threshold=0.5)] == ["same"]
    with pytest.raises(VectorGraphError):
        g.nearest((1.0, 0.0, 0.0), k=0)


def test_construction_and_lookup_validation() -> None:
    with pytest.raises(VectorGraphError):
        VectorGraph(dim=0)
    with pytest.raises(VectorGraphError):
        VectorGraph(dim=3, capacity=0)
    with pytest.raises(VectorGraphError, match="no node"):
        _g().get("nope")
    with pytest.raises(VectorGraphError, match="requires a key"):
        _g().add("  ", (1.0, 0.0, 0.0))


# ==========================================================================
# #64 population tournament
# ==========================================================================
def _t(n: int = 4, **kw) -> Tournament:
    t = Tournament(**kw)
    for i in range(n):
        t.enter(Variant(f"v{i}"))
    return t


def test_schedule_is_deterministic_round_robin() -> None:
    a, b = Tournament(), Tournament()
    for v in ("v2", "v0", "v1"):
        a.enter(Variant(v))
    for v in ("v1", "v2", "v0"):
        b.enter(Variant(v))
    assert a.schedule() == b.schedule()
    assert a.schedule() == [("v0", "v1"), ("v0", "v2"), ("v1", "v2")]


def test_a_variant_that_never_played_is_unranked_not_last() -> None:
    """'Untested' and 'tested and bad' are different states; collapsing them
    lets a variant win by having avoided scrutiny."""
    t = _t(3)
    for i in range(MIN_MATCHES):
        t.record("v0", "v1", 1.0, 0.0, task_id=f"t{i}")
    table = {s.vid: s for s in t.standings()}
    assert table["v0"].ranked and table["v0"].played == MIN_MATCHES
    assert not table["v2"].ranked and table["v2"].played == 0
    assert "unranked" in table["v2"].reason
    assert "v2" in {s.vid for s in t.standings()}, "unranked must not be dropped"


def test_champion_requires_enough_matches() -> None:
    t = _t(2)
    t.record("v0", "v1", 1.0, 0.0)
    assert t.champion() is None, "one match is not a tournament"
    for i in range(MIN_MATCHES):
        t.record("v0", "v1", 1.0, 0.0, task_id=f"t{i}")
    assert t.champion().vid == "v0"


def test_a_tie_does_not_promote() -> None:
    """Same rule as SkillOpt's acceptance bar: promoting on a draw changes the
    incumbent with no evidence anything improved."""
    t = _t(2)
    for i in range(MIN_MATCHES + 1):
        t.record("v0", "v1", 1.0, 1.0, task_id=f"t{i}")
    assert t.champion() is None
    table = {s.vid: s for s in t.standings()}
    assert table["v0"].draws == MIN_MATCHES + 1
    assert table["v0"].points == table["v1"].points


def test_draw_has_no_winner_and_is_never_index_order() -> None:
    assert Match("a", "b", 1.0, 1.0).winner is None
    assert Match("a", "b", 2.0, 1.0).winner == "a"
    assert Match("a", "b", 1.0, 2.0).winner == "b"


def test_both_scores_are_retained() -> None:
    """A rank from win counts alone cannot tell a near-miss from a rout."""
    t = _t(2)
    m = t.record("v0", "v1", 0.99, 1.0)
    assert m.score_a == 0.99 and m.score_b == 1.0


def test_points_tally_wins_and_draws() -> None:
    t = _t(2)
    t.record("v0", "v1", 1.0, 0.0)
    t.record("v0", "v1", 0.0, 1.0)
    t.record("v0", "v1", 1.0, 1.0)
    table = {s.vid: s for s in t.standings()}
    assert table["v0"].points == 1.5 and table["v1"].points == 1.5
    assert table["v0"].wins == 1 and table["v0"].losses == 1


def test_duplicate_entry_and_self_play_are_refused() -> None:
    t = _t(2)
    with pytest.raises(TournamentError, match="already entered"):
        t.enter(Variant("v0"))
    with pytest.raises(TournamentError, match="cannot play itself"):
        t.record("v0", "v0", 1.0, 0.0)
    with pytest.raises(TournamentError, match="not entered"):
        t.record("v0", "ghost", 1.0, 0.0)


def test_population_of_one_is_refused() -> None:
    """It always produces a winner and never produces information."""
    t = Tournament()
    t.enter(Variant("only"))
    with pytest.raises(TournamentError, match="at least two"):
        t.schedule()


def test_variant_and_tournament_validation() -> None:
    with pytest.raises(TournamentError, match="requires an id"):
        Variant("  ")
    with pytest.raises(TournamentError, match="min_matches"):
        Tournament(min_matches=0)


def test_empty_tournament_has_no_champion() -> None:
    assert Tournament().champion() is None


# ==========================================================================
# #65 replayable run visualiser
# ==========================================================================
def _trajectory(**extra) -> io.StringIO:
    buf = io.StringIO()
    w = TrajectoryWriter(buf, "run-42")
    w.append("start", model="local-7b")
    # Prefix assembled, never a literal — see the note in test_budget.py.
    w.append("call", api_key=("gh" + "p_") + "z" * 20,
             prompt="<script>alert(1)</script>")
    w.append("done", **extra) if extra else w.append("done", ok=True)
    buf.seek(0)
    return buf


def test_render_is_deterministic() -> None:
    """'Replayable' is the requirement; a page with a render timestamp is not."""
    assert render_html(_trajectory()) == render_html(_trajectory())


def test_render_never_un_redacts() -> None:
    """A viewer reaching for the raw log would undo write-time scrubbing on a
    page someone screenshots."""
    page = render_html(_trajectory())
    assert ("gh" + "p_") not in page
    assert REDACTED in page


def test_untrusted_fields_are_html_escaped() -> None:
    """Agent output routinely contains markup; unescaped it is stored XSS."""
    page = render_html(_trajectory())
    assert "<script>alert(1)</script>" not in page
    assert "&lt;script&gt;" in page


def test_page_is_self_contained_no_network() -> None:
    """On an offline OS a fetching viewer breaks exactly when it is needed, and
    under privacy mode it is also an egress attempt."""
    page = render_html(_trajectory())
    for marker in ("http://", "https://", "//cdn", "<script", "@import", "src="):
        assert marker not in page, marker


def test_summary_counts_events() -> None:
    buf = _trajectory()
    from aether.agents.budget.trajectory import read_trajectory

    summary = render_summary(list(read_trajectory(buf)))
    assert summary["run_id"] == "run-42"
    assert summary["events"] == 3
    assert summary["by_event"] == {"call": 1, "done": 1, "start": 1}
    # Dict equality ignores ORDER, so the line above does not test the sort at
    # all. This does. Until DDR-856 it was missing, and a mutation removing the
    # sort survived — a page whose event summary reorders between runs is not
    # "replayable", which is the one property #65 asks for.
    assert list(summary["by_event"]) == ["call", "done", "start"]


def test_empty_trajectory_is_refused() -> None:
    """An empty page would look like a run that did nothing rather than a file
    that was not found."""
    with pytest.raises(VisualiserError, match="empty trajectory"):
        render_html(io.StringIO(""))


def test_mixed_run_ids_are_refused() -> None:
    """Rendering them as one run would invent a sequence that never happened."""
    with pytest.raises(VisualiserError, match="mixes run ids"):
        render_summary([{"run_id": "a", "seq": 1, "event": "x"},
                        {"run_id": "b", "seq": 1, "event": "y"}])


def test_truncated_tail_still_renders() -> None:
    """It reuses the writer's own reader, so it inherits that tolerance rather
    than re-implementing the parse and drifting from it."""
    buf = _trajectory()
    mangled = io.StringIO(buf.getvalue() + '{"run_id":"run-42","seq":4,"ev')
    assert "run-42" in render_html(mangled)
