"""Guards around SkillOpt: validation, sleep, transfer (3D #47-49, DDR-848).

Every test here is a REFUSAL. That is the shape of the slice: SkillOpt can
already improve a skill, and these three modules exist to bound what "improve"
is allowed to mean when the thing being rewritten is the document that says what
the agent may do.
"""
from __future__ import annotations

import pytest

from aether.agents.skillopt import Reflection, Rollout, SkillOptLoop
from aether.agents.skillopt.sleep import (
    JOURNAL_CAPACITY,
    MIN_TRAIN_ROLLOUTS,
    SkillOptSleep,
    heldout_split,
)
from aether.agents.skillopt.transfer import propose_transfer
from aether.agents.skillopt.validate import (
    MIN_REFUSALS,
    SkillValidationError,
    count_refusals,
    parse_capabilities,
    validate_skill_update,
)

# A minimal but STRUCTURALLY REAL skill body: same fields and sections the
# committed roster files carry, so the validator is exercised against the shape
# it will actually see rather than a convenient stub.
BASE = """# TESTAGENT — `test_agent`

- **Role:** `test_agent`
- **Capabilities:** CAP_AGENT, CAP_MEMORY
- **Status:** live

## Role

The test agent exists to exercise the validator against a document with the
same structure as a real roster entry, because a validator verified only
against a stub is a validator verified against nothing in particular. It reads,
it records, and it declines the things listed below without exception.

## What it does

- Reads a named file and returns its contents, bounded by the payload ceiling.
- Records durable facts in the agent memory store, one fact per entry, so that
  a later retrieval returns something that can be checked rather than a summary
  that cannot.
- Submits every action through the approval queue, including the ones it
  expects to be auto-approved, because an action that skips the queue is an
  action with no audit record and therefore no way to answer what happened.

## How it decides

Prefer the smallest action that accomplishes the request. When an instruction is
ambiguous, ask rather than guess: a wrong target is not a failed action, it is a
successful action against the wrong thing, and the audit record will show it
looking entirely correct in isolation.

## Refuses

- Raising its own memory or capability limits, under any framing. (S1.)
- Approving its own proposed actions. (S4 — the human gate is structural.)
- Writing to a CAP_SOVEREIGN-locked path, including the objective function. (S3.)

## Invariants

Bound by the kernel invariants S1-S8 and the host invariants S1-S14, which
collide in label only and must never be merged. Every action is submitted
through the AETHER action queue and lands in the SHA-256 chained audit log,
which this agent cannot erase or amend because no user-space erase path exists
and the build fails if one is added. Padding this section to clear the token
floor is deliberate: the floor is part of what is being tested here.
"""


def _swap(text: str, old: str, new: str) -> str:
    assert old in text, "fixture drift: %r not in BASE" % old
    return text.replace(old, new)


# ==========================================================================
# #48 validation — the escalation gate
# ==========================================================================
def test_baseline_fixture_is_itself_valid() -> None:
    """If BASE did not pass, every rejection below would prove nothing."""
    report = validate_skill_update(BASE, BASE)
    assert report.capabilities == frozenset({"CAP_AGENT", "CAP_MEMORY"})
    assert report.refusals == 3


def test_candidate_cannot_grant_itself_a_new_capability() -> None:
    """THE test. Privilege escalation through prose."""
    evil = _swap(BASE, "CAP_AGENT, CAP_MEMORY", "CAP_AGENT, CAP_MEMORY, CAP_SOVEREIGN")
    with pytest.raises(SkillValidationError, match="not granted"):
        validate_skill_update(BASE, evil)


def test_capabilities_may_shrink() -> None:
    """Dropping a capability is always safe and must not be blocked."""
    lesser = _swap(BASE, "CAP_AGENT, CAP_MEMORY", "CAP_AGENT")
    assert validate_skill_update(BASE, lesser).capabilities == frozenset({"CAP_AGENT"})


def test_granted_set_overrides_the_incumbent_declaration() -> None:
    """An agent may legitimately declare less than the kernel gave it."""
    more = _swap(BASE, "CAP_AGENT, CAP_MEMORY", "CAP_AGENT, CAP_MEMORY, CAP_NET")
    with pytest.raises(SkillValidationError):
        validate_skill_update(BASE, more)
    ok = validate_skill_update(
        BASE, more, granted={"CAP_AGENT", "CAP_MEMORY", "CAP_NET"}
    )
    assert "CAP_NET" in ok.capabilities


def test_refusal_named_in_prose_is_not_read_as_a_grant() -> None:
    """The failure mode that would punish the most careful files.

    BASE's Refuses section names CAP_SOVEREIGN in order to DECLINE it. A
    whole-document capability scan would read that as a claim to hold it.
    """
    assert "CAP_SOVEREIGN" in BASE
    assert "CAP_SOVEREIGN" not in parse_capabilities(BASE)
    validate_skill_update(BASE, BASE)  # must not raise


def test_candidate_cannot_thin_its_refusals() -> None:
    """The edit the optimiser is most rewarded for making."""
    weakened = _swap(
        BASE, "- Approving its own proposed actions. (S4 — the human gate is structural.)\n", ""
    )
    with pytest.raises(SkillValidationError, match="drops refusals"):
        validate_skill_update(BASE, weakened)


def test_candidate_may_add_refusals() -> None:
    stricter = _swap(
        BASE, "## Invariants", "- Reading files outside the operator's tree.\n\n## Invariants"
    )
    assert validate_skill_update(BASE, stricter).refusals == 4


def test_candidate_cannot_delete_the_invariants_section() -> None:
    gutted = BASE.split("## Invariants", 1)[0]
    with pytest.raises(SkillValidationError, match="Invariants"):
        validate_skill_update(BASE, gutted)


def test_candidate_cannot_drop_a_required_field() -> None:
    norole = _swap(BASE, "- **Role:** `test_agent`\n", "")
    with pytest.raises(SkillValidationError, match="Role"):
        validate_skill_update(BASE, norole)


def test_missing_capabilities_line_is_refused_not_assumed_empty() -> None:
    """Refusing to check is not the same as checking and finding nothing."""
    nocaps = _swap(BASE, "- **Capabilities:** CAP_AGENT, CAP_MEMORY\n", "")
    with pytest.raises(SkillValidationError, match="Capabilities"):
        validate_skill_update(BASE, nocaps)


def test_unwired_capability_needs_the_spawnable_marker() -> None:
    with pytest.raises(SkillValidationError, match="unwired"):
        validate_skill_update(
            BASE,
            _swap(BASE, "CAP_AGENT, CAP_MEMORY", "CAP_AGENT, CAP_OCR"),
            granted={"CAP_AGENT", "CAP_OCR"},
        )


def test_unwired_capability_is_allowed_when_marked() -> None:
    marked = _swap(BASE, "**Status:** live", "**Status:** not yet spawnable")
    marked = _swap(marked, "CAP_AGENT, CAP_MEMORY", "CAP_AGENT, CAP_OCR")
    validate_skill_update(BASE, marked, granted={"CAP_AGENT", "CAP_OCR"})


def test_oversized_candidate_is_refused() -> None:
    bloated = BASE + ("\n- filler word repeated to blow the ceiling" * 400)
    with pytest.raises(SkillValidationError, match="budget"):
        validate_skill_update(BASE, bloated)


def test_empty_candidate_is_refused() -> None:
    with pytest.raises(SkillValidationError, match="empty"):
        validate_skill_update(BASE, "   ")


def test_count_refusals_stops_at_the_next_section() -> None:
    """Bullets under Invariants must not be counted as refusals."""
    padded = _swap(BASE, "## Invariants\n", "## Invariants\n\n- not a refusal\n")
    assert count_refusals(padded) == 3


# ==========================================================================
# #47 sleep — deterministic split, same acceptance bar, validation first
# ==========================================================================
def _scorer_prefers_learned(body: str, _r: Rollout) -> float:
    return 0.9 if "## Learned" in body else 0.5


def _rollouts(n: int, ok: bool = False) -> list[Rollout]:
    return [Rollout(f"task{i}", ok, 1.0 if ok else 0.0) for i in range(n)]


def test_split_is_deterministic_and_order_independent() -> None:
    """An unreproducible rejection gets re-run until it passes."""
    rs = _rollouts(60)
    a_tr, a_ho = heldout_split(rs)
    b_tr, b_ho = heldout_split(list(reversed(rs)))
    assert {r.task_id for r in a_ho} == {r.task_id for r in b_ho}
    assert {r.task_id for r in a_tr} == {r.task_id for r in b_tr}


def test_split_is_disjoint_and_total() -> None:
    rs = _rollouts(60)
    tr, ho = heldout_split(rs)
    assert not ({r.task_id for r in tr} & {r.task_id for r in ho})
    assert len(tr) + len(ho) == len(rs)


def test_split_actually_holds_some_out() -> None:
    """A split that put everything in training would pass the tests above."""
    tr, ho = heldout_split(_rollouts(60))
    assert len(ho) >= 8 and len(tr) >= 20


def test_salt_changes_the_split() -> None:
    a = {r.task_id for r in heldout_split(_rollouts(60))[1]}
    b = {r.task_id for r in heldout_split(_rollouts(60), salt="x")[1]}
    assert a != b


def test_recording_never_revises_the_skill() -> None:
    """Waking operation must not move the thing being measured."""
    s = SkillOptSleep(BASE, _scorer_prefers_learned)
    for r in _rollouts(40):
        s.record(r)
    assert s.incumbent == BASE


def test_consolidate_refuses_when_there_is_too_little_to_learn_from() -> None:
    s = SkillOptSleep(BASE, _scorer_prefers_learned)
    for r in _rollouts(MIN_TRAIN_ROLLOUTS - 1):
        s.record(r)
    res = s.consolidate()
    assert not res.applied
    assert s.incumbent == BASE


def test_consolidate_applies_on_a_strictly_positive_improvement() -> None:
    s = SkillOptSleep(BASE, _scorer_prefers_learned)
    for r in _rollouts(40):
        s.record(r)
    res = s.consolidate()
    assert res.applied, res.reason
    assert "## Learned" in s.incumbent
    assert res.train_n >= MIN_TRAIN_ROLLOUTS


def test_sleep_uses_the_same_bar_a_tie_still_loses() -> None:
    """No relaxed offline threshold. This is the point of the module."""
    s = SkillOptSleep(BASE, lambda _b, _r: 0.7)
    for r in _rollouts(40):
        s.record(r)
    res = s.consolidate()
    assert not res.applied
    assert "tie" in res.reason.lower()
    assert s.incumbent == BASE


def test_consolidate_runs_validation_before_scoring() -> None:
    """A candidate that escalates must not reach a comparison it could win.

    The scorer here loves ANY candidate, so if validation ran after scoring — or
    not at all — this would be adopted.
    """
    s = SkillOptSleep(BASE, lambda b, _r: 1.0 if "## Learned" in b else 0.0)
    # Granting nothing at all makes the incumbent's own declared caps an
    # escalation, so the candidate cannot pass validation however well it scores.
    s._granted = frozenset()
    for r in _rollouts(40):
        s.record(r)
    res = s.consolidate()
    assert not res.applied
    assert "validation" in res.reason
    assert s.incumbent == BASE


def test_journal_is_bounded_and_counts_what_it_dropped() -> None:
    s = SkillOptSleep(BASE, _scorer_prefers_learned, capacity=10)
    for i in range(25):
        s.record(Rollout(f"t{i}", False, 0.0))
    assert len(s.journal) == 10
    assert s.dropped == 15
    assert s.journal[0].task_id == "t15", "the OLDEST must be dropped"


def test_default_journal_capacity_is_bounded() -> None:
    assert 0 < JOURNAL_CAPACITY < 1_000_000


def test_zero_capacity_is_refused() -> None:
    with pytest.raises(ValueError):
        SkillOptSleep(BASE, _scorer_prefers_learned, capacity=0)


# ==========================================================================
# #49 transfer — a proposal, never an application
# ==========================================================================
def test_transfer_returns_proposals_and_applies_nothing() -> None:
    lessons = [Reflection("t1", "gap: batch small writes", 1.0)]
    report = propose_transfer("KRYOS", lessons, {"CAP_AGENT", "CAP_MEMORY"})
    assert len(report.accepted) == 1
    assert isinstance(report.accepted[0].as_reflection(), Reflection)


def test_transferred_lesson_is_discounted_below_first_hand_evidence() -> None:
    lessons = [Reflection("t1", "gap: batch small writes", 1.0)]
    got = propose_transfer("KRYOS", lessons, {"CAP_AGENT"}).accepted[0]
    assert got.weight == 0.5


def test_lesson_needing_a_capability_the_recipient_lacks_is_not_transferred() -> None:
    """Teaching a technique the kernel will deny just moves the failure later."""
    lessons = [Reflection("t1", "use CAP_NET to fetch the manifest first", 1.0)]
    report = propose_transfer("LUMYN", lessons, {"CAP_AGENT", "CAP_MEMORY"})
    assert not report.accepted
    assert len(report.rejected) == 1
    assert "CAP_NET" in report.rejected[0][1]


def test_transfer_preserves_provenance() -> None:
    lessons = [Reflection("t42", "gap: parsing", 1.0)]
    got = propose_transfer("PRAX", lessons, {"CAP_AGENT"}).accepted[0]
    assert got.source_agent == "PRAX"
    assert got.source_tasks == ("t42",)


def test_transferred_task_id_cannot_masquerade_as_a_local_rollout() -> None:
    """Namespacing is what stops a transfer colliding with the recipient's own.

    A collision would make the held-out disjointness check in run() fire on a
    task the recipient never ran, or worse, silently pass.
    """
    lessons = [Reflection("t1", "gap: parsing", 1.0)]
    refl = propose_transfer("PRAX", lessons, {"CAP_AGENT"}).accepted[0].as_reflection()
    assert refl.task_id == "PRAX:t1"


def test_unnamed_source_is_refused() -> None:
    with pytest.raises(ValueError, match="provenance"):
        propose_transfer("  ", [], {"CAP_AGENT"})


def test_out_of_range_discount_is_refused() -> None:
    for bad in (0.0, -1.0, 1.5):
        with pytest.raises(ValueError):
            propose_transfer("KRYOS", [], {"CAP_AGENT"}, discount=bad)


def test_transferred_lessons_survive_selection_against_local_ones() -> None:
    """A discounted lesson must still be able to win if it is genuinely heavier."""
    local = [Reflection("l1", "gap: minor", 0.2)]
    transferred = propose_transfer(
        "KRYOS", [Reflection("t1", "gap: major", 4.0)], {"CAP_AGENT"}
    ).accepted
    merged = SkillOptLoop.aggregate(local + [p.as_reflection() for p in transferred])
    assert merged[0].lesson == "gap: major"


def test_min_refusals_constant_matches_the_roster_gate() -> None:
    """DDR-846's test and this validator must not drift apart."""
    assert MIN_REFUSALS == 3
