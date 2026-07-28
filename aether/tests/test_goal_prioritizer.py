"""D-12 discriminating gate — adaptive goal prioritizer."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.agents.planner.adaptive_goal_prioritizer import (
    MIN_ADAPT_SAMPLES,
    URGENCY_HORIZON_S,
    WEIGHT_MAX,
    AdaptiveGoalPrioritizer,
    Goal,
    PrioritizerError,
    Weights,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox

NOW = 1_000_000.0
DAY = 24 * 3600.0


def _prioritizer(tmp_path: Path, granted: bool = True, tap=None, max_goals=4096,
                 weights: Weights | None = None):
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("D-12", "adaptive_goal_prioritizer.py", "CAP_SUBSTRATE")
    return AdaptiveGoalPrioritizer(
        lockbox=lb, weights=weights, store_path=tmp_path / "goals.jsonl",
        audit=audit, clock=lambda: NOW, alignment_tap=tap, max_goals=max_goals,
    )


def test_recency_alone_is_not_priority(tmp_path: Path) -> None:
    """THE discriminator.

    The naive queue is a stack: whatever arrived last is worked first because
    it is the most salient. That is indistinguishable from responsiveness for
    about a day, after which the important-but-old work has been pushed down by
    a drip of trivia and is never reached.

    Here the trivial goal is BOTH the newest and cheapest — everything recency
    can offer — and must still lose to valuable, deadline-bearing work.
    """
    p = _prioritizer(tmp_path)
    p.add(Goal("important", "ship the safety review", value=0.95,
               cost=0.4, deadline=NOW + 2 * DAY, created_ts=NOW - 20 * DAY))
    p.add(Goal("trivia", "rename a variable", value=0.05, cost=0.0,
               created_ts=NOW))                       # newest possible

    ordered = p.prioritize()
    assert [s.goal.goal_id for s in ordered] == ["important", "trivia"]
    assert p.next_goal().goal_id == "important"       # type: ignore[union-attr]
    # And recency really did favour the loser — the trap is live, not absent.
    assert ordered[1].terms["recency"] > ordered[0].terms["recency"]


def test_recency_still_breaks_a_genuine_tie(tmp_path: Path) -> None:
    """Recency must count for something, or the term is decoration."""
    p = _prioritizer(tmp_path)
    p.add(Goal("older", "a", value=0.5, created_ts=NOW - 5 * DAY))
    p.add(Goal("newer", "b", value=0.5, created_ts=NOW))
    assert [s.goal.goal_id for s in p.prioritize()] == ["newer", "older"]


def test_blocked_goal_loses_its_readiness_term(tmp_path: Path) -> None:
    """A blocked goal at the front stalls everything behind it, however
    valuable it is."""
    p = _prioritizer(tmp_path)
    p.add(Goal("blocked", "high value but waiting", value=1.0,
               blocked_by=("dep-1",), created_ts=NOW))
    p.add(Goal("ready", "modest but actionable", value=0.6, created_ts=NOW))
    assert p.next_goal().goal_id == "ready"           # type: ignore[union-attr]

    p.unblock("blocked", "dep-1")
    assert p.next_goal().goal_id == "blocked"         # type: ignore[union-attr]


def test_urgency_rises_toward_the_deadline(tmp_path: Path) -> None:
    p = _prioritizer(tmp_path)
    far = Goal("far", "x", value=0.5, deadline=NOW + 6 * DAY, created_ts=NOW)
    near = Goal("near", "y", value=0.5, deadline=NOW + 1 * DAY, created_ts=NOW)
    p.add(far)
    p.add(near)
    assert p.score(near).terms["urgency"] > p.score(far).terms["urgency"]
    assert [s.goal.goal_id for s in p.prioritize()] == ["near", "far"]


def test_deadline_beyond_the_horizon_and_already_passed(tmp_path: Path) -> None:
    p = _prioritizer(tmp_path)
    beyond = Goal("beyond", "x", deadline=NOW + 2 * URGENCY_HORIZON_S)
    passed = Goal("passed", "y", deadline=NOW - DAY)
    none_set = Goal("none", "z")
    assert p.score(beyond).terms["urgency"] == 0.0
    assert p.score(passed).terms["urgency"] == pytest.approx(1.0)
    assert p.score(none_set).terms["urgency"] == 0.0


def test_cost_subtracts(tmp_path: Path) -> None:
    p = _prioritizer(tmp_path)
    p.add(Goal("cheap", "x", value=0.7, cost=0.0, created_ts=NOW))
    p.add(Goal("dear", "y", value=0.7, cost=1.0, created_ts=NOW))
    assert [s.goal.goal_id for s in p.prioritize()] == ["cheap", "dear"]
    assert p.score(p.goals[1]).terms["cost"] < 0.0


def test_completed_goals_leave_the_queue(tmp_path: Path) -> None:
    p = _prioritizer(tmp_path)
    p.add(Goal("g1", "x", value=0.9, created_ts=NOW))
    p.add(Goal("g2", "y", value=0.1, created_ts=NOW))
    p.complete("g1")
    assert [s.goal.goal_id for s in p.prioritize()] == ["g2"]
    assert len(p.prioritize(include_done=True)) == 2


def test_ordering_is_deterministic_on_exact_ties(tmp_path: Path) -> None:
    """S9 — a tie must not resolve by dict insertion order."""
    forward = _prioritizer(tmp_path / "f")
    reverse = _prioritizer(tmp_path / "r")
    ids = ["b", "a", "c"]
    for gid in ids:
        forward.add(Goal(gid, "x", value=0.5, created_ts=NOW))
    for gid in reversed(ids):
        reverse.add(Goal(gid, "x", value=0.5, created_ts=NOW))
    assert [s.goal.goal_id for s in forward.prioritize()] == \
           [s.goal.goal_id for s in reverse.prioritize()] == ["a", "b", "c"]


def test_adaptation_needs_a_sample_not_a_lucky_run(tmp_path: Path) -> None:
    p = _prioritizer(tmp_path)
    for i in range(MIN_ADAPT_SAMPLES - 1):
        p.add(Goal(f"g{i}", "x"))
        p.complete(f"g{i}", success=True)
    with pytest.raises(PrioritizerError, match="below the adaptation floor"):
        p.adapt()


def test_adaptation_leans_on_readiness_when_failing(tmp_path: Path) -> None:
    """Doing badly should bias toward work that can actually be finished."""
    p = _prioritizer(tmp_path)
    before = p.weights.readiness
    for i in range(MIN_ADAPT_SAMPLES):
        p.add(Goal(f"g{i}", "x"))
        p.complete(f"g{i}", success=False)
    after = p.adapt()
    assert after.readiness > before
    assert after.value < 1.0


def test_adaptation_leans_back_to_value_when_succeeding(tmp_path: Path) -> None:
    p = _prioritizer(tmp_path)
    for i in range(MIN_ADAPT_SAMPLES):
        p.add(Goal(f"g{i}", "x"))
        p.complete(f"g{i}", success=True)
    after = p.adapt()
    assert after.value > 1.0
    assert after.readiness < 1.0


def test_weights_stay_clamped_under_repeated_adaptation(tmp_path: Path) -> None:
    """No term may run away and collapse the ordering onto itself."""
    p = _prioritizer(tmp_path)
    for i in range(MIN_ADAPT_SAMPLES):
        p.add(Goal(f"g{i}", "x"))
        p.complete(f"g{i}", success=True)
    for _ in range(200):
        w = p.adapt(step=0.5)
    assert w.value == WEIGHT_MAX
    assert all(0.0 < v <= WEIGHT_MAX for v in
               (w.value, w.urgency, w.readiness, w.recency, w.cost))


def test_queue_is_bounded_and_evictions_logged(tmp_path: Path) -> None:
    """S2 — the oldest goal goes, and it is not lost silently."""
    p = _prioritizer(tmp_path, max_goals=3)
    for i in range(5):
        p.add(Goal(f"g{i}", "x", created_ts=NOW + i))
    assert [g.goal_id for g in p.goals] == ["g2", "g3", "g4"]
    drops = [e for e in AuditLog(tmp_path / "audit_log.jsonl").read_all()
             if e["action"] == "goal.evicted"]
    assert len(drops) == 2
    assert drops[0]["extra"]["reason"]


@pytest.mark.parametrize(
    ("goal", "match"),
    [
        (Goal("", "x"), "needs an id"),
        (Goal("g", "x", value=2.0), r"value 2.0 outside \[0, 1\]"),
        (Goal("g", "x", cost=-1.0), r"cost -1.0 outside \[0, 1\]"),
    ],
)
def test_malformed_goals_refused(tmp_path: Path, goal, match) -> None:
    with pytest.raises(PrioritizerError, match=match):
        _prioritizer(tmp_path).add(goal)


def test_duplicate_id_refused(tmp_path: Path) -> None:
    p = _prioritizer(tmp_path)
    p.add(Goal("g", "x"))
    with pytest.raises(PrioritizerError, match="duplicate goal id"):
        p.add(Goal("g", "y"))


def test_unknown_goal_refused(tmp_path: Path) -> None:
    p = _prioritizer(tmp_path)
    with pytest.raises(PrioritizerError, match="unknown goal"):
        p.complete("nope")


def test_empty_queue_has_no_next_goal(tmp_path: Path) -> None:
    assert _prioritizer(tmp_path).next_goal() is None


def test_capability_required(tmp_path: Path) -> None:
    p = _prioritizer(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_SUBSTRATE"):
        p.add(Goal("g", "x"))


def test_alignment_tap_receives_steps(tmp_path: Path) -> None:
    events: list[str] = []
    p = _prioritizer(tmp_path, tap=lambda s, _d: events.append(s))
    p.add(Goal("g", "x"))
    p.prioritize()
    for i in range(MIN_ADAPT_SAMPLES):
        p.add(Goal(f"h{i}", "x"))
        p.complete(f"h{i}", success=True)
    p.adapt()
    assert {"D-12.add", "D-12.prioritize", "D-12.adapt"} <= set(events)


def test_initial_weights_are_clamped(tmp_path: Path) -> None:
    p = _prioritizer(tmp_path, weights=Weights(value=999.0, cost=-5.0))
    assert p.weights.value == WEIGHT_MAX
    assert p.weights.cost > 0.0
