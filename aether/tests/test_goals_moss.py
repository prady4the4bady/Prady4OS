"""goals.md, subconscious loop, MOSS pipeline (3D #53-55, DDR-851)."""
from __future__ import annotations

import pytest

from aether.agents.goals import (
    MAX_ACTIVE_GOALS,
    GoalError,
    GoalStatus,
    parse_goals,
)
from aether.agents.goals.subconscious import (
    SUBCONSCIOUS_SYSCALL_SHARE,
    SYSCALL_LIMIT_PER_S,
    RateLimitExceeded,
    SubconsciousLoop,
)
from aether.agents.moss import (
    CAP_REWRITE,
    CAP_SOVEREIGN,
    MossError,
    MossPipeline,
    RegressionResult,
    RewriteProposal,
    Stage,
)

SOV = {"CAP_SOVEREIGN"}

GOALS_MD = """# KRYOS goals

## Durability priority: 10

- [ ] (G1) Never lose an operator file | success: zero data-loss incidents in the audit log
- [~] (G2) Keep writes minimal | success: mean bytes written per request falls vs last week

## Hygiene priority: 3

- [x] (G3) Document every refusal | success: skill.md Refuses section lists >= 3 items
"""


# ==========================================================================
# #53 goals.md
# ==========================================================================
def test_parses_ids_statuses_and_priorities() -> None:
    gs = parse_goals("KRYOS", GOALS_MD, approver_caps=SOV)
    assert [g.gid for g in gs.by_priority()] == ["G1", "G2", "G3"]
    assert gs.get("G2").status is GoalStatus.IN_PROGRESS
    assert gs.get("G3").status is GoalStatus.COMPLETE
    assert gs.get("G1").priority == 10 and gs.get("G3").priority == 3
    assert len(gs.active) == 2


def test_goals_require_sovereign() -> None:
    """The objective function is not something the agent may edit."""
    with pytest.raises(GoalError, match="CAP_SOVEREIGN"):
        parse_goals("KRYOS", GOALS_MD)
    with pytest.raises(GoalError, match="CAP_SOVEREIGN"):
        parse_goals("KRYOS", GOALS_MD, approver_caps={"CAP_AGENT"})


def test_goal_without_a_success_criterion_is_refused() -> None:
    """THE rule. An unfalsifiable goal makes every report against it unfalsifiable."""
    bad = "- [ ] (G1) Improve file handling\n"
    with pytest.raises(GoalError, match="malformed goal line"):
        parse_goals("KRYOS", bad, approver_caps=SOV)


def test_empty_criterion_is_refused_at_the_type_level() -> None:
    from aether.agents.goals import Goal

    with pytest.raises(GoalError, match="no success criterion"):
        Goal("G1", "do a thing", "   ")


def test_malformed_line_is_rejected_not_skipped() -> None:
    """A skipped line is a goal that silently stopped existing."""
    text = GOALS_MD + "- [ ] G4 missing parens | success: something\n"
    with pytest.raises(GoalError, match="malformed goal line"):
        parse_goals("KRYOS", text, approver_caps=SOV)


def test_duplicate_goal_ids_are_refused() -> None:
    text = GOALS_MD + "- [ ] (G1) another | success: something checkable\n"
    with pytest.raises(GoalError, match="duplicate goal id"):
        parse_goals("KRYOS", text, approver_caps=SOV)


def test_empty_goals_file_is_refused() -> None:
    """Almost always a format error, not an agent with nothing to do."""
    with pytest.raises(GoalError, match="no goals parsed"):
        parse_goals("KRYOS", "# nothing here\n", approver_caps=SOV)


def test_too_many_active_goals_is_refused() -> None:
    lines = "\n".join(
        f"- [ ] (G{i}) goal {i} | success: criterion {i}"
        for i in range(MAX_ACTIVE_GOALS + 1)
    )
    with pytest.raises(GoalError, match="pursuing none of them"):
        parse_goals("KRYOS", lines, approver_caps=SOV)


def test_completed_goals_do_not_count_against_the_active_cap() -> None:
    lines = "\n".join(
        f"- [x] (G{i}) goal {i} | success: criterion {i}"
        for i in range(MAX_ACTIVE_GOALS + 5)
    )
    assert len(parse_goals("K", lines, approver_caps=SOV).active) == 0


def test_agent_cannot_mark_its_own_goal_complete() -> None:
    """S4, in the place it would be least visible."""
    g = parse_goals("KRYOS", GOALS_MD, approver_caps=SOV).get("G1")
    with pytest.raises(GoalError, match="requires an attestor"):
        g.complete("")
    g.complete("SOLIN")
    assert g.status is GoalStatus.COMPLETE and g.completed_by == "SOLIN"


def test_diff_returns_unmet_goals_in_priority_order() -> None:
    gs = parse_goals("KRYOS", GOALS_MD, approver_caps=SOV)
    assert [g.gid for g in gs.diff_against(set())] == ["G1", "G2"]
    assert [g.gid for g in gs.diff_against({"G1"})] == ["G2"]


def test_unnamed_agent_is_refused() -> None:
    with pytest.raises(GoalError, match="named agent"):
        parse_goals("  ", GOALS_MD, approver_caps=SOV)


# ==========================================================================
# #54 subconscious loop
# ==========================================================================
def _gs():
    return parse_goals("KRYOS", GOALS_MD, approver_caps=SOV)


def test_does_not_fire_before_its_period_elapses() -> None:
    loop = SubconsciousLoop(period=1000)
    assert loop.wake(0, _gs(), set(), {"KRYOS"})
    assert loop.wake(500, _gs(), set(), {"KRYOS"}) == []
    assert loop.wake(1000, _gs(), set(), {"KRYOS"})


def test_skips_busy_agents() -> None:
    """Prompting a busy agent interrupts work on the strength of a goal it may
    already be pursuing."""
    loop = SubconsciousLoop(period=10)
    assert loop.wake(0, _gs(), set(), idle_agents=set()) == []
    assert loop.skipped_busy == ["KRYOS"]


def test_prompts_carry_the_goal_that_justifies_them() -> None:
    """A prompt with no goal behind it cannot be reviewed."""
    prompts = SubconsciousLoop(period=10).wake(0, _gs(), set(), {"KRYOS"})
    assert {p.goal_id for p in prompts} == {"G1", "G2"}
    assert all(p.agent == "KRYOS" for p in prompts)
    assert "Success is:" in prompts[0].text


def test_emits_nothing_when_every_goal_is_achieved() -> None:
    assert SubconsciousLoop(period=10).wake(0, _gs(), {"G1", "G2"}, {"KRYOS"}) == []


def test_prompt_count_is_bounded_per_wake() -> None:
    loop = SubconsciousLoop(period=10, max_prompts_per_wake=1)
    assert len(loop.wake(0, _gs(), set(), {"KRYOS"})) == 1


def test_loop_stays_under_a_minority_share_of_the_syscall_limit() -> None:
    """Background work that eats the budget makes the AGENT look slow."""
    loop = SubconsciousLoop()
    assert loop.budget_per_second == int(SYSCALL_LIMIT_PER_S * SUBCONSCIOUS_SYSCALL_SHARE)
    assert loop.budget_per_second < SYSCALL_LIMIT_PER_S // 2


def test_exceeding_the_share_raises_rather_than_truncating_the_batch() -> None:
    """A truncated batch silently drops the lowest-priority goals."""
    loop = SubconsciousLoop(period=1, max_prompts_per_wake=3, syscalls_per_prompt=6)
    loop.wake(0, _gs(), set(), {"KRYOS"})  # 2 goals * 6 = 12 of 15
    with pytest.raises(RateLimitExceeded, match="partial batch"):
        loop.wake(1, _gs(), set(), {"KRYOS"})


def test_budget_window_slides_so_the_loop_is_not_permanently_wedged() -> None:
    loop = SubconsciousLoop(period=1, max_prompts_per_wake=3, syscalls_per_prompt=6)
    loop.wake(0, _gs(), set(), {"KRYOS"})
    assert loop.wake(1000, _gs(), set(), {"KRYOS"}), "a new second is a new budget"


def test_rejects_nonsense_configuration() -> None:
    for kwargs in ({"period": 0}, {"max_prompts_per_wake": 0}, {"syscalls_per_prompt": 0}):
        with pytest.raises(ValueError):
            SubconsciousLoop(**kwargs)
    with pytest.raises(ValueError):
        SubconsciousLoop().due(-1)


def test_loop_has_no_execution_path() -> None:
    """It proposes. A background loop that can act is an unsupervised agent
    running on a timer."""
    for forbidden in ("execute", "run_action", "apply", "submit", "dispatch"):
        assert not hasattr(SubconsciousLoop, forbidden)


# ==========================================================================
# #55 MOSS pipeline
# ==========================================================================
def _proposal(**kw) -> RewriteProposal:
    base = dict(
        target_path="agents/kryos.py",
        original="def f():\n    return 1\n",
        proposed="def f():\n    return 2\n",
        author_agent="RUFLO",
    )
    base.update(kw)
    return RewriteProposal(**base)


def _pipeline_to_snapshot(pipe: MossPipeline, prop: RewriteProposal) -> None:
    pipe.install(prop.target_path, prop.original)
    pipe.stage(prop)
    pipe.regress(prop, RegressionResult(True, 40, 0))
    pipe.approve(prop, "HERMES", {CAP_REWRITE})
    pipe.approve(prop, "OWNER", {CAP_SOVEREIGN})
    pipe.snapshot(prop)


def test_full_happy_path_promotes_and_can_roll_back() -> None:
    pipe, prop = MossPipeline(), _proposal()
    _pipeline_to_snapshot(pipe, prop)
    pipe.promote(prop)
    assert prop.stage is Stage.PROMOTED
    assert pipe.read(prop.target_path) == prop.proposed
    pipe.rollback(prop)
    assert pipe.read(prop.target_path) == prop.original
    assert prop.stage is Stage.ROLLED_BACK


def test_staging_never_touches_the_live_tree() -> None:
    pipe, prop = MossPipeline(), _proposal()
    pipe.install(prop.target_path, prop.original)
    pipe.stage(prop)
    assert pipe.read(prop.target_path) == prop.original
    assert prop.staged_path and prop.target_path in prop.staged_path


def test_a_suite_that_never_RAN_is_not_a_pass() -> None:
    """THE guard. `failures == 0` is also true when zero tests ran."""
    assert not RegressionResult(ran=False, total=0, failures=0).passed
    assert not RegressionResult(ran=True, total=0, failures=0).passed
    assert RegressionResult(ran=True, total=1, failures=0).passed

    pipe, prop = MossPipeline(), _proposal()
    pipe.install(prop.target_path, prop.original)
    pipe.stage(prop)
    assert not pipe.regress(prop, RegressionResult(False, 0, 0))
    assert prop.stage is Stage.DISCARDED


def test_failed_regression_discards_and_blocks_promotion() -> None:
    pipe, prop = MossPipeline(), _proposal()
    pipe.install(prop.target_path, prop.original)
    pipe.stage(prop)
    assert not pipe.regress(prop, RegressionResult(True, 40, 3))
    with pytest.raises(MossError):
        pipe.snapshot(prop)
    assert pipe.read(prop.target_path) == prop.original


def test_regression_must_run_against_the_staged_candidate() -> None:
    """Running it against the live tree tests the code already there."""
    pipe, prop = MossPipeline(), _proposal()
    with pytest.raises(MossError, match="STAGED"):
        pipe.regress(prop, RegressionResult(True, 40, 0))


def test_promote_without_a_snapshot_is_structurally_impossible() -> None:
    pipe, prop = MossPipeline(), _proposal()
    pipe.install(prop.target_path, prop.original)
    pipe.stage(prop)
    pipe.regress(prop, RegressionResult(True, 40, 0))
    with pytest.raises(MossError, match="cannot promote"):
        pipe.promote(prop)


def test_rollback_without_a_snapshot_raises() -> None:
    with pytest.raises(MossError, match="no snapshot"):
        MossPipeline().rollback(_proposal())


def test_co_approval_requires_both_capabilities() -> None:
    pipe, prop = MossPipeline(), _proposal()
    pipe.install(prop.target_path, prop.original)
    pipe.stage(prop)
    pipe.regress(prop, RegressionResult(True, 40, 0))
    pipe.approve(prop, "HERMES", {CAP_REWRITE})
    pipe.snapshot(prop)
    with pytest.raises(MossError, match="CAP_SOVEREIGN=NO"):
        pipe.promote(prop)


def test_one_principal_holding_both_caps_is_not_co_approval() -> None:
    """One decision wearing two hats is what a two-key rule exists to prevent."""
    pipe, prop = MossPipeline(), _proposal()
    pipe.install(prop.target_path, prop.original)
    pipe.stage(prop)
    pipe.regress(prop, RegressionResult(True, 40, 0))
    pipe.approve(prop, "OWNER", {CAP_REWRITE, CAP_SOVEREIGN})
    pipe.snapshot(prop)
    with pytest.raises(MossError, match="TWO DIFFERENT principals"):
        pipe.promote(prop)


def test_author_cannot_approve_its_own_rewrite() -> None:
    prop = _proposal()
    with pytest.raises(MossError, match="cannot approve it"):
        MossPipeline().approve(prop, "RUFLO", {CAP_SOVEREIGN})


def test_one_principal_cannot_approve_twice() -> None:
    prop = _proposal()
    pipe = MossPipeline()
    pipe.approve(prop, "HERMES", {CAP_REWRITE})
    with pytest.raises(MossError, match="already approved"):
        pipe.approve(prop, "HERMES", {CAP_SOVEREIGN})


def test_noop_rewrite_is_refused() -> None:
    """It still consumes an approval and lands in the audit log as a change."""
    with pytest.raises(MossError, match="identical"):
        _proposal(proposed="def f():\n    return 1\n")


def test_unattributed_or_empty_rewrite_is_refused() -> None:
    with pytest.raises(MossError, match="author"):
        _proposal(author_agent="  ")
    with pytest.raises(MossError, match="empty"):
        _proposal(proposed="   ")
    with pytest.raises(MossError, match="target path"):
        _proposal(target_path="")


def test_stages_cannot_be_replayed_out_of_order() -> None:
    pipe, prop = MossPipeline(), _proposal()
    _pipeline_to_snapshot(pipe, prop)
    with pytest.raises(MossError, match="cannot stage"):
        pipe.stage(prop)
