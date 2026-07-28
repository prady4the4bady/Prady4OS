"""D-15 discriminating gate — autonomous skill composer."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.agents.tool_composer.autonomous_skill_composer import (
    MAX_STEPS,
    AutonomousSkillComposer,
    CapError,
    Skill,
    SkillComposerError,
    SkillNotRegistered,
    SkillStatus,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _composer(tmp_path: Path, granted: bool = True, tap=None, agent_id="D-15-agent"):
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("D-15", "autonomous_skill_composer.py", "CAP_ONTOLOGY_WRITE")
    comp = AutonomousSkillComposer(
        lockbox=lb, agent_id=agent_id, store_path=tmp_path / "skills.jsonl",
        audit=audit, alignment_tap=tap,
    )
    return comp


def _seed(comp: AutonomousSkillComposer) -> None:
    comp.register_skill(Skill("read_file", "core", ("CAP_MEMORY_WRITE",)))
    comp.register_skill(Skill("summarise", "core", ()))


def test_unregistered_step_is_refused_at_compose_time(tmp_path: Path) -> None:
    """THE discriminator (half one).

    A composer that may name a step which does not exist can invent capability
    by writing a string: the skill reviews as a legitimate plan and only
    discovers at execution that it calls something nobody declared. So the
    refusal must land at composition, and nothing may be stored.
    """
    comp = _composer(tmp_path)
    _seed(comp)
    with pytest.raises(SkillNotRegistered, match="unregistered step"):
        comp.compose("digest", ["read_file", "hallucinated_step"])
    assert comp.get("digest") is None
    assert comp.drafts() == []
    assert not (tmp_path / "skills.jsonl").exists()


def test_self_approval_raises_cap_error(tmp_path: Path) -> None:
    """THE discriminator (half two) — S1.

    Self-approval is the shortest path from "the system proposes" to "the
    system does". Every other control here — C-08 review, D-08 monitoring, the
    lockbox — assumes an approval means a second party looked, and a composer
    that can approve its own work makes all of them decorative without ever
    touching the lockbox.
    """
    comp = _composer(tmp_path, agent_id="D-15-agent")
    _seed(comp)
    comp.compose("digest", ["read_file", "summarise"])
    with pytest.raises(CapError, match="self-approval refused"):
        comp.approve("digest", approver="D-15-agent")
    assert comp.get("digest").status is SkillStatus.DRAFT   # type: ignore[union-attr]
    assert comp.runnable("digest") is False


def test_second_party_approval_is_accepted(tmp_path: Path) -> None:
    """The guard must not be uselessly strict."""
    comp = _composer(tmp_path, agent_id="D-15-agent")
    _seed(comp)
    comp.compose("digest", ["read_file", "summarise"])
    approved = comp.approve("digest", approver="C-08-reviewer")
    assert approved.status is SkillStatus.APPROVED
    assert approved.approved_by == "C-08-reviewer"
    assert comp.runnable("digest") is True


def test_cap_error_is_the_lockbox_error_type() -> None:
    assert CapError is CapabilityError


def test_draft_is_not_runnable(tmp_path: Path) -> None:
    """A draft is a proposal, not a permission."""
    comp = _composer(tmp_path)
    _seed(comp)
    comp.compose("digest", ["read_file"])
    assert comp.runnable("digest") is False
    assert comp.runnable("never composed") is False


def test_composed_capabilities_are_the_union_and_no_more(tmp_path: Path) -> None:
    """S8 — composition must not be a capability-laundering route: chaining two
    harmless steps must not yield a skill claiming something neither held."""
    comp = _composer(tmp_path)
    comp.register_skill(Skill("a", "core", ("CAP_MEMORY_WRITE",)))
    comp.register_skill(Skill("b", "core", ("CAP_WATCHDOG",)))
    comp.register_skill(Skill("harmless", "core", ()))

    both = comp.compose("ab", ["a", "b"])
    assert set(both.capabilities) == {"CAP_MEMORY_WRITE", "CAP_WATCHDOG"}

    none = comp.compose("hh", ["harmless", "harmless"])
    assert none.capabilities == ()          # nothing conjured from nothing


def test_self_approval_refusal_is_audited(tmp_path: Path) -> None:
    comp = _composer(tmp_path, agent_id="D-15-agent")
    _seed(comp)
    comp.compose("digest", ["read_file"])
    with pytest.raises(CapError):
        comp.approve("digest", approver="D-15-agent")
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "D-15"
               and e["action"] == "skill.self_approval_refused" for e in entries)


def test_original_composer_cannot_approve_even_from_another_instance(
    tmp_path: Path,
) -> None:
    """The check is on who COMPOSED it, not merely on the current agent id —
    otherwise the same agent launders approval through a fresh instance."""
    comp = _composer(tmp_path, agent_id="D-15-agent")
    _seed(comp)
    comp.compose("digest", ["read_file"])
    with pytest.raises(CapError, match="self-approval refused"):
        comp.approve("digest", approver=comp.get("digest").composed_by)  # type: ignore[union-attr]


def test_unregistered_step_refusal_is_audited(tmp_path: Path) -> None:
    comp = _composer(tmp_path)
    _seed(comp)
    with pytest.raises(SkillNotRegistered):
        comp.compose("x", ["nope"])
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["action"] == "skill.unregistered_step" for e in entries)


def test_duplicate_and_empty_compositions_refused(tmp_path: Path) -> None:
    comp = _composer(tmp_path)
    _seed(comp)
    comp.compose("digest", ["read_file"])
    with pytest.raises(SkillComposerError, match="already exists"):
        comp.compose("digest", ["summarise"])
    with pytest.raises(SkillComposerError, match="has no steps"):
        comp.compose("empty", [])
    with pytest.raises(SkillComposerError, match="needs a name"):
        comp.compose("", ["read_file"])


def test_step_cap_enforced(tmp_path: Path) -> None:
    """S2."""
    comp = _composer(tmp_path)
    _seed(comp)
    with pytest.raises(SkillComposerError, match="exceeds the cap"):
        comp.compose("huge", ["read_file"] * (MAX_STEPS + 1))


def test_rejection_requires_a_reason(tmp_path: Path) -> None:
    comp = _composer(tmp_path)
    _seed(comp)
    comp.compose("digest", ["read_file"])
    with pytest.raises(SkillComposerError, match="needs a reason"):
        comp.reject("digest", "C-08", "   ")
    rejected = comp.reject("digest", "C-08", "unsafe step ordering")
    assert rejected.status is SkillStatus.REJECTED
    assert comp.runnable("digest") is False


def test_unknown_composition_refused(tmp_path: Path) -> None:
    comp = _composer(tmp_path)
    with pytest.raises(SkillComposerError, match="unknown composed skill"):
        comp.approve("ghost", "someone")


def test_approval_needs_an_approver(tmp_path: Path) -> None:
    comp = _composer(tmp_path)
    _seed(comp)
    comp.compose("digest", ["read_file"])
    with pytest.raises(SkillComposerError, match="needs an approver"):
        comp.approve("digest", approver="")


def test_composition_is_deterministic(tmp_path: Path) -> None:
    """S9 — capability order must not depend on set iteration."""
    a = _composer(tmp_path / "a")
    b = _composer(tmp_path / "b")
    for comp in (a, b):
        comp.register_skill(Skill("x", "core", ("CAP_WATCHDOG", "CAP_MEMORY_WRITE")))
        comp.register_skill(Skill("y", "core", ("CAP_SUBSTRATE",)))
    assert a.compose("c", ["x", "y"]).capabilities == \
           b.compose("c", ["x", "y"]).capabilities


def test_registry_listing(tmp_path: Path) -> None:
    comp = _composer(tmp_path)
    _seed(comp)
    assert comp.registered() == ["read_file", "summarise"]
    with pytest.raises(SkillComposerError, match="needs a name"):
        comp.register_skill(Skill("", "core"))


def test_capability_required(tmp_path: Path) -> None:
    comp = _composer(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_ONTOLOGY_WRITE"):
        comp.register_skill(Skill("x", "core"))


def test_alignment_tap_receives_steps(tmp_path: Path) -> None:
    events: list[str] = []
    comp = _composer(tmp_path, tap=lambda s, _d: events.append(s))
    _seed(comp)
    comp.compose("digest", ["read_file"])
    comp.approve("digest", "C-08")
    assert {"D-15.register", "D-15.compose", "D-15.approve"} <= set(events)
