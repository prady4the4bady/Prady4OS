"""I-02 gate — capability enforcement, and the world-model write path."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.capability.enforcement import (
    CAP_AGENT,
    CAP_SOVEREIGN,
    CapabilityEnforcer,
    CapError,
    EnforcementError,
    OperationRule,
    Principal,
    Tier,
)
from aether.kernel.audit.audit_log import AuditLog


def _enforcer(tmp_path: Path) -> CapabilityEnforcer:
    enf = CapabilityEnforcer(audit=AuditLog(tmp_path / "audit_log.jsonl"))
    enf.register_defaults()
    return enf


_AGENT = Principal("worker-1", Tier.AGENT, frozenset({CAP_AGENT}))
_SOVEREIGN = Principal("kernel-1", Tier.SOVEREIGN,
                       frozenset({CAP_AGENT, CAP_SOVEREIGN}))
_OBSERVER = Principal("watcher-1", Tier.OBSERVER, frozenset())


def test_cap_agent_world_model_write_is_refused_cleanly(tmp_path: Path) -> None:
    """THE I-02 discriminator.

    A CAP_AGENT process writing shared world state without CAP_SOVEREIGN must
    get a clean CapError — never a silent success. The module may legitimately
    hold CAP_WORLD_MODEL_WRITE; the *principal* running it still may not mutate
    state every other agent reads.
    """
    enf = _enforcer(tmp_path)
    with pytest.raises(CapError, match="world_model.write"):
        enf.check(_AGENT, "world_model.write", resource="entity:alpha")


def test_sovereign_world_model_write_is_allowed(tmp_path: Path) -> None:
    """The layer must not be uselessly strict."""
    assert _enforcer(tmp_path).check(_SOVEREIGN, "world_model.write") is True


def test_denial_is_audited_with_a_reason(tmp_path: Path) -> None:
    """A denial nobody can explain later is barely better than a silent one."""
    enf = _enforcer(tmp_path)
    with pytest.raises(CapError):
        enf.check(_AGENT, "world_model.write", resource="entity:alpha")
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    denials = [e for e in entries if e["action"] == "capability.deny"]
    assert len(denials) == 1
    assert denials[0]["extra"]["principal"] == "worker-1"
    assert denials[0]["extra"]["operation"] == "world_model.write"
    assert denials[0]["extra"]["reason"]


def test_allowals_are_audited_too(tmp_path: Path) -> None:
    """Recording only denials makes 'what did this principal actually do'
    unanswerable, which is the question that matters after an incident."""
    enf = _enforcer(tmp_path)
    enf.check(_SOVEREIGN, "world_model.write")
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["action"] == "capability.allow" for e in entries)


def test_unregistered_operation_is_denied_not_allowed(tmp_path: Path) -> None:
    """Fail-closed. A default-allow layer ships every future operation
    unprotected while the suite stays green — the gap is invisible until
    something uses it."""
    enf = _enforcer(tmp_path)
    with pytest.raises(CapError, match="no rule registered"):
        enf.check(_SOVEREIGN, "operation.invented.today")
    # Even a sovereign principal does not get an unregistered operation.
    assert enf.allowed(_SOVEREIGN, "operation.invented.today") is False


def test_tier_below_requirement_is_refused(tmp_path: Path) -> None:
    enf = _enforcer(tmp_path)
    with pytest.raises(CapError, match="below required"):
        enf.check(_OBSERVER, "failure.record")


def test_missing_capability_is_refused_even_at_the_right_tier(
    tmp_path: Path,
) -> None:
    """Tier alone is not authority — the capability set is checked too."""
    enf = _enforcer(tmp_path)
    tierless_sovereign = Principal("odd", Tier.SOVEREIGN, frozenset())
    with pytest.raises(CapError, match="missing capabilities"):
        enf.check(tierless_sovereign, "world_model.write")


def test_reads_are_open_to_observers(tmp_path: Path) -> None:
    assert _enforcer(tmp_path).check(_OBSERVER, "world_model.read") is True


def test_per_agent_operations_do_not_need_sovereign(tmp_path: Path) -> None:
    """Recording one's own failure and proposing a hypothesis are not acts on
    shared state — requiring SOVEREIGN there would push agents to skip the
    registry entirely."""
    enf = _enforcer(tmp_path)
    assert enf.check(_AGENT, "failure.record") is True
    assert enf.check(_AGENT, "hypothesis.generate") is True


def test_skill_approval_requires_sovereign(tmp_path: Path) -> None:
    """Reinforces D-15's S1 rule at the principal level."""
    enf = _enforcer(tmp_path)
    with pytest.raises(CapError):
        enf.check(_AGENT, "skill.approve")
    assert enf.check(_SOVEREIGN, "skill.approve") is True


def test_allowed_is_a_probe_not_enforcement(tmp_path: Path) -> None:
    enf = _enforcer(tmp_path)
    assert enf.allowed(_AGENT, "world_model.write") is False
    assert enf.allowed(_SOVEREIGN, "world_model.write") is True


def test_guard_decorator_blocks_before_the_call_runs(tmp_path: Path) -> None:
    """Checking after the side effect would be no check at all."""
    enf = _enforcer(tmp_path)
    ran: list[str] = []

    @enf.guard(_AGENT, "world_model.write", "entity:alpha")
    def mutate() -> str:
        ran.append("yes")
        return "done"

    with pytest.raises(CapError):
        mutate()
    assert ran == [], "the guarded body executed despite the denial"


def test_guard_allows_an_authorised_principal(tmp_path: Path) -> None:
    enf = _enforcer(tmp_path)

    @enf.guard(_SOVEREIGN, "world_model.write")
    def mutate() -> str:
        return "done"

    assert mutate() == "done"


def test_sovereign_property_needs_both_tier_and_capability() -> None:
    assert _SOVEREIGN.sovereign is True
    assert Principal("p", Tier.SOVEREIGN, frozenset()).sovereign is False
    assert Principal("p", Tier.AGENT,
                     frozenset({CAP_SOVEREIGN})).sovereign is False


def test_tiers_are_ordered() -> None:
    assert Tier.OBSERVER < Tier.AGENT < Tier.SOVEREIGN


def test_rule_needs_an_operation_name(tmp_path: Path) -> None:
    enf = _enforcer(tmp_path)
    with pytest.raises(EnforcementError, match="needs an operation name"):
        enf.register_rule(OperationRule("", Tier.AGENT))


def test_defaults_cover_the_shipped_mutating_operations(tmp_path: Path) -> None:
    """If a shipped mutation has no rule, fail-closed turns it into a runtime
    denial rather than a review finding — so the roster is pinned here."""
    assert set(_enforcer(tmp_path).rules()) >= {
        "world_model.write", "knowledge.consolidate", "failure.record",
        "hypothesis.generate", "skill.approve", "world_model.read",
    }


def test_world_model_write_end_to_end(tmp_path: Path) -> None:
    """The I-02 wire itself: enforcement in front of a real D-04 write.

    An AGENT principal is stopped before the world model is touched, and a
    SOVEREIGN one gets through — with the lockbox still doing its own check
    underneath.
    """
    from aether.agents.world_model.world_model import Entity, WorldModel
    from aether.kernel.lockbox.cap_sovereign import Lockbox

    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    lb.register("D-04", "world_model.py", "CAP_WORLD_MODEL_WRITE")
    wm = WorldModel(lockbox=lb, entities_path=tmp_path / "ents.jsonl",
                    relations_path=tmp_path / "rels.jsonl", audit=audit)
    enf = CapabilityEnforcer(audit=audit)
    enf.register_defaults()

    def write(principal: Principal, entity_id: str) -> None:
        enf.check(principal, "world_model.write", resource=entity_id)
        wm.add_entity(Entity(entity_id, name=entity_id, entity_type="node"),
                      agent_ddr="D-04")

    with pytest.raises(CapError):
        write(_AGENT, "blocked-entity")
    assert wm.get("blocked-entity") is None      # nothing was written

    write(_SOVEREIGN, "allowed-entity")
    assert wm.get("allowed-entity") is not None
