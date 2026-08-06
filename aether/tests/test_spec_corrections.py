"""Corrections bringing #47/#48/#50/#52 to spec (DDR-850).

Four items shipped in DDR-848/849 against my reading of the item titles rather
than the spec text in `docs/AETHER_MASTER_FEATURES.md`. Re-reading it found:

  #50 is "TokenJuice CONTEXT COMPRESSION (<=80% tokens)" — I built a budget
  #48 is a validation gate "CAP_SOVEREIGN ALWAYS"      — I built structure only
  #47 is "harvest->mine->replay->consolidate; PAUSES ACTIVE AGENTS"
  #52 pairs token_count with LATENCY_MS

These tests cover the parts that were missing.
"""
from __future__ import annotations

import pytest

from aether.agents.budget.compress import (
    TARGET_FRACTION,
    CompressionImpossible,
    Segment,
    compress,
)
from aether.agents.budget.cost import CostLedger, ModelRate
from aether.agents.skillopt import Rollout
from aether.agents.skillopt.sleep import SkillOptSleep
from aether.agents.skillopt.validate import SkillValidationError, validate_skill_update
from aether.tests.test_skillopt_guards import BASE

SOV = {"CAP_SOVEREIGN"}


# ==========================================================================
# #48 — CAP_SOVEREIGN always
# ==========================================================================
def test_structurally_perfect_update_still_needs_sovereign() -> None:
    """"Always" means a flawless candidate is refused too, not just a bad one."""
    with pytest.raises(SkillValidationError, match="CAP_SOVEREIGN"):
        validate_skill_update(BASE, BASE, approver_caps={"CAP_AGENT"})


def test_missing_approver_is_refused_not_assumed_authorised() -> None:
    """The permissive reading of a forgotten argument is an ungated rewrite."""
    with pytest.raises(SkillValidationError, match="CAP_SOVEREIGN"):
        validate_skill_update(BASE, BASE)


def test_sovereign_approver_passes() -> None:
    assert validate_skill_update(BASE, BASE, approver_caps=SOV).refusals == 3


def test_sovereign_does_not_excuse_a_bad_candidate() -> None:
    """Authority and validity are separate questions; both must hold."""
    evil = BASE.replace("CAP_AGENT, CAP_MEMORY", "CAP_AGENT, CAP_MEMORY, CAP_SOVEREIGN")
    with pytest.raises(SkillValidationError, match="not granted"):
        validate_skill_update(BASE, evil, approver_caps=SOV)


# ==========================================================================
# #47 — pauses active agents, and the four named stages exist
# ==========================================================================
def _rollouts(n: int) -> list[Rollout]:
    return [Rollout(f"task{i}", False, 0.0) for i in range(n)]


def _scorer(body: str, _r: Rollout) -> float:
    return 0.9 if "## Learned" in body else 0.5


def test_consolidation_refuses_while_an_agent_is_active() -> None:
    """Rewriting a skill mid-task means neither skill describes what ran."""
    s = SkillOptSleep(BASE, _scorer, granted_caps={"CAP_AGENT", "CAP_MEMORY"})
    for r in _rollouts(40):
        s.record(r)
    s.mark_active("KRYOS")
    res = s.consolidate(approver_caps=SOV)
    assert not res.applied
    assert "still active" in res.reason
    assert s.incumbent == BASE


def test_pausing_then_consolidating_succeeds_and_resume_restores() -> None:
    s = SkillOptSleep(BASE, _scorer, granted_caps={"CAP_AGENT", "CAP_MEMORY"})
    for r in _rollouts(40):
        s.record(r)
    s.mark_active("KRYOS")
    s.mark_active("PRAX")
    paused = s.pause_agents()
    assert paused == frozenset({"KRYOS", "PRAX"})
    assert s.active_agents == frozenset()

    res = s.consolidate(approver_caps=SOV)
    assert res.applied, res.reason

    s.resume_agents(paused)
    assert s.active_agents == frozenset({"KRYOS", "PRAX"})
    assert not s.paused


def test_consolidation_still_needs_sovereign_approval() -> None:
    """The sleep path must not be a way around #48."""
    s = SkillOptSleep(BASE, _scorer, granted_caps={"CAP_AGENT", "CAP_MEMORY"})
    for r in _rollouts(40):
        s.record(r)
    res = s.consolidate()  # no approver
    assert not res.applied
    assert "CAP_SOVEREIGN" in res.reason
    assert s.incumbent == BASE


def test_the_four_named_stages_are_separately_callable() -> None:
    """harvest -> mine -> replay -> consolidate, as the spec names them."""
    s = SkillOptSleep(BASE, _scorer, granted_caps={"CAP_AGENT", "CAP_MEMORY"})
    for r in _rollouts(40):
        s.record(r)
    train, held = s.harvest()
    assert train and held
    lessons = s.mine(train)
    assert lessons
    from aether.agents.skillopt import SkillOptLoop

    verdict = s.replay(SkillOptLoop.update(BASE, lessons), held)
    assert verdict.accepted
    assert s.incumbent == BASE, "replay must not mutate; only consolidate applies"


# ==========================================================================
# #50 — context compression to <=80%
# ==========================================================================
def _seg(key: str, words: int, **kw) -> Segment:
    return Segment(key, " ".join([key] * words), **kw)


def test_compresses_to_at_most_the_target_fraction() -> None:
    segs = [_seg(f"s{i}", 100, seq=i) for i in range(10)]
    res = compress(segs)
    assert res.fraction <= TARGET_FRACTION
    assert res.final_tokens <= int(res.original_tokens * TARGET_FRACTION)
    assert res.dropped


def test_pinned_segments_are_never_dropped() -> None:
    segs = [_seg("system", 100, pinned=True, seq=0)] + [
        _seg(f"s{i}", 100, seq=i) for i in range(1, 10)
    ]
    res = compress(segs)
    assert "system" in {s.key for s in res.segments}
    assert "system" not in {s.key for s in res.dropped}


def test_impossible_target_raises_rather_than_returning_best_effort() -> None:
    """A best-effort context is indistinguishable from a successful one."""
    segs = [_seg("system", 500, pinned=True, seq=0), _seg("chat", 10, seq=1)]
    with pytest.raises(CompressionImpossible, match="Refusing to return"):
        compress(segs)


def test_eviction_prefers_low_priority_then_old() -> None:
    segs = [
        _seg("old_important", 100, priority=9, seq=0),
        _seg("old_trivial", 100, priority=0, seq=1),
        _seg("new_trivial", 100, priority=0, seq=2),
    ]
    dropped = {s.key for s in compress(segs, 0.7).dropped}
    assert dropped == {"old_trivial"}, "lowest priority, oldest first"


def test_compression_is_deterministic() -> None:
    """An irreproducible bad answer gets retried rather than fixed."""
    segs = [_seg(f"s{i}", 50 + i, priority=i % 3, seq=i) for i in range(20)]
    a = [s.key for s in compress(segs).segments]
    b = [s.key for s in compress(list(reversed(segs))).segments]
    assert a == b


def test_output_order_follows_arrival_not_eviction_ranking() -> None:
    segs = [_seg(f"s{i}", 60, priority=(9 - i), seq=i) for i in range(6)]
    kept = [s.seq for s in compress(segs).segments]
    assert kept == sorted(kept)


def test_duplicate_keys_are_refused() -> None:
    with pytest.raises(ValueError, match="duplicate segment keys"):
        compress([_seg("dup", 10, seq=0), _seg("dup", 10, seq=1)])


def test_empty_context_is_not_an_error() -> None:
    res = compress([])
    assert res.segments == [] and res.original_tokens == 0


def test_render_joins_only_survivors() -> None:
    segs = [_seg("keep", 10, pinned=True, seq=0), _seg("drop", 300, seq=1)]
    res = compress(segs)
    assert "drop" not in res.render()
    assert "keep" in res.render()


def test_bad_target_fraction_is_refused() -> None:
    for bad in (0.0, -0.5, 1.5):
        with pytest.raises(ValueError):
            compress([_seg("a", 10, seq=0)], bad)


# ==========================================================================
# #52 — latency_ms alongside token_count
# ==========================================================================
RATES = {"big": ModelRate(300, 1500)}


def test_latency_is_recorded_and_averaged() -> None:
    ledger = CostLedger(RATES)
    ledger.record("KRYOS", "big", 100, 100, latency_ms=200)
    ledger.record("KRYOS", "big", 100, 100, latency_ms=400)
    totals = ledger.by_agent()["KRYOS"]
    assert totals.latency_ms == 600
    assert totals.mean_latency_ms == 300.0


def test_latency_appears_in_the_model_view_too() -> None:
    ledger = CostLedger(RATES)
    ledger.record("KRYOS", "big", 10, 10, latency_ms=50)
    assert ledger.by_model()["big"].latency_ms == 50


def test_negative_latency_is_refused() -> None:
    with pytest.raises(ValueError, match="latency"):
        CostLedger(RATES).record("KRYOS", "big", 1, 1, latency_ms=-1)


def test_mean_latency_of_an_unused_agent_is_zero_not_a_crash() -> None:
    from aether.agents.budget.cost import _AgentTotals

    assert _AgentTotals().mean_latency_ms == 0.0
