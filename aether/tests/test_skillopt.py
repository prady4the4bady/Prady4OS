"""SkillOpt acceptance-rule tests (Section 3D #46, DDR-847).

The tests that matter are the REJECTIONS. A training loop that accepts good
candidates is easy; one that reliably refuses ties, regressions, self-scored
candidates and small samples is the thing that stops a skill drifting somewhere
nobody chose.
"""
from __future__ import annotations

import pytest

from aether.agents.skillopt import (
    MIN_HELDOUT_ROLLOUTS,
    Reflection,
    Rollout,
    SkillCandidate,
    SkillOptLoop,
)


def _rollouts(prefix: str, n: int, ok: bool = True) -> list[Rollout]:
    return [Rollout(f"{prefix}{i}", ok, 1.0 if ok else 0.0) for i in range(n)]


def _scorer_prefers_learned(body: str, _r: Rollout) -> float:
    """A candidate carrying a '## Learned' section scores higher."""
    return 0.9 if "## Learned" in body else 0.5


def _scorer_flat(_body: str, _r: Rollout) -> float:
    return 0.7


def _scorer_prefers_incumbent(body: str, _r: Rollout) -> float:
    return 0.2 if "## Learned" in body else 0.8


# --------------------------------------------------------------------------
# acceptance
# --------------------------------------------------------------------------
def test_accepts_only_on_strictly_positive_improvement() -> None:
    loop = SkillOptLoop("base skill", _scorer_prefers_learned)
    res = loop.run(_rollouts("train", 4, ok=False), _rollouts("held", 6))
    assert res.accepted
    assert res.improvement > 0
    assert loop.incumbent != "base skill"


def test_tie_is_rejected_not_accepted() -> None:
    """The case an implementation gets wrong by writing >= and never noticing."""
    loop = SkillOptLoop("base skill", _scorer_flat)
    res = loop.run(_rollouts("train", 4, ok=False), _rollouts("held", 6))
    assert not res.accepted
    assert res.improvement == 0
    assert "tie" in res.reason.lower()
    assert loop.incumbent == "base skill", "a tie must not mutate the incumbent"


def test_regression_is_rejected() -> None:
    loop = SkillOptLoop("base skill", _scorer_prefers_incumbent)
    res = loop.run(_rollouts("train", 4, ok=False), _rollouts("held", 6))
    assert not res.accepted
    assert res.improvement < 0
    assert loop.incumbent == "base skill"


def test_small_heldout_set_is_refused_even_when_better() -> None:
    """A score from too few samples is noise; accepting on it is gambling."""
    loop = SkillOptLoop("base skill", _scorer_prefers_learned)
    n = MIN_HELDOUT_ROLLOUTS - 1
    res = loop.run(_rollouts("train", 4, ok=False), _rollouts("held", n))
    assert not res.accepted
    assert "too small" in res.reason
    assert loop.incumbent == "base skill"


def test_heldout_overlapping_training_is_rejected_loudly() -> None:
    """Scoring a candidate on its own homework is the S7 failure."""
    loop = SkillOptLoop("base skill", _scorer_prefers_learned)
    shared = _rollouts("same", 6, ok=False)
    with pytest.raises(ValueError, match="own homework"):
        loop.run(shared, shared)


# --------------------------------------------------------------------------
# the pipeline stages
# --------------------------------------------------------------------------
def test_reflect_weights_failures_above_successes() -> None:
    """A failure is the only signal that says where the skill does NOT reach."""
    refl = SkillOptLoop.reflect([Rollout("a", True, 1.0), Rollout("b", False, 0.0)])
    by_task = {r.task_id: r for r in refl}
    assert by_task["b"].weight > by_task["a"].weight


def test_aggregate_merges_duplicate_lessons_and_sums_weight() -> None:
    """A lesson learned five times is one lesson with more evidence."""
    refl = [Reflection("t1", "gap: x", 1.0), Reflection("t2", "gap: x", 1.0)]
    merged = SkillOptLoop.aggregate(refl)
    assert len(merged) == 1
    assert merged[0].weight == 2.0


def test_aggregate_orders_by_weight_descending() -> None:
    refl = [Reflection("t1", "light", 0.5), Reflection("t2", "heavy", 3.0)]
    assert SkillOptLoop.aggregate(refl)[0].lesson == "heavy"


def test_select_is_bounded() -> None:
    refl = [Reflection(f"t{i}", f"lesson{i}", float(i)) for i in range(10)]
    assert len(SkillOptLoop.select(SkillOptLoop.aggregate(refl), k=3)) == 3


def test_select_rejects_non_positive_k() -> None:
    with pytest.raises(ValueError):
        SkillOptLoop.select([], k=0)


def test_update_without_lessons_returns_incumbent_unchanged() -> None:
    cand = SkillOptLoop.update("base", [])
    assert cand.body == "base"


# --------------------------------------------------------------------------
# invariants of the types themselves
# --------------------------------------------------------------------------
def test_rollout_score_is_bounded() -> None:
    with pytest.raises(ValueError):
        Rollout("t", True, 1.5)
    with pytest.raises(ValueError):
        Rollout("t", True, -0.1)


def test_empty_incumbent_is_refused() -> None:
    with pytest.raises(ValueError):
        SkillOptLoop("   ", _scorer_flat)


def test_result_always_carries_the_numbers_behind_the_verdict() -> None:
    """An acceptance an operator cannot audit is a change for unstated reasons."""
    loop = SkillOptLoop("base skill", _scorer_prefers_learned)
    res = loop.run(_rollouts("train", 4, ok=False), _rollouts("held", 6))
    assert res.heldout_n == 6
    assert res.incumbent_score != res.candidate_score
    assert res.reason


def test_history_records_rejections_too() -> None:
    """Rejected attempts are evidence; a loop that logs only wins hides drift."""
    loop = SkillOptLoop("base skill", _scorer_flat)
    loop.run(_rollouts("a", 4, ok=False), _rollouts("h1", 6))
    loop.run(_rollouts("b", 4, ok=False), _rollouts("h2", 6))
    assert len(loop.history) == 2
    assert not any(r.accepted for r in loop.history)


def test_candidate_records_provenance() -> None:
    lessons = [Reflection("t7", "gap: parsing", 1.0)]
    cand: SkillCandidate = SkillOptLoop.update("base", lessons)
    assert cand.derived_from == ("t7",)
