"""Section G — the 12-agent roster with real behaviour (DDR-857)."""
from __future__ import annotations

import importlib

import pytest

from aether.agents.runtime.roles import (
    ROSTER,
    UNWIRED_CAPS,
    CapabilityRefused,
    NotSpawnable,
    Role,
    RoleAgent,
    RosterError,
    build_roster,
)

LEGACY_SLOTS = {"KRYOS", "PRAX", "LUMYN", "AHNIS", "IRIS", "RUFLO", "HERMES", "SOLIN"}


# ==========================================================================
# roster shape
# ==========================================================================
def test_roster_has_twelve_roles() -> None:
    assert len(ROSTER) == 12


def test_role_names_are_unique() -> None:
    """Two agents sharing a role means one of them has no reason to exist."""
    names = [r.name for r in ROSTER]
    assert len(names) == len(set(names))


def test_the_eight_legacy_slots_are_covered_exactly_once() -> None:
    """DDR-846's decision: the legacy names BECOME the first eight roles rather
    than sitting alongside them, so the roster extends one working set."""
    slots = [r.slot for r in ROSTER if r.slot is not None]
    assert set(slots) == LEGACY_SLOTS
    assert len(slots) == len(set(slots)) == 8


def test_the_four_new_roles_have_no_kernel_slot_yet() -> None:
    unslotted = [r.name for r in ROSTER if r.slot is None]
    assert sorted(unslotted) == [
        "ai_scientist_agent", "architect_agent", "subconscious_agent",
        "tournament_agent",
    ]


# ==========================================================================
# the rule this file exists to enforce
# ==========================================================================
def test_every_role_names_a_delegate() -> None:
    """THE rule. A role that implements its own reasoning duplicates a
    subsystem and wins by being imported first — the DDR-855 failure."""
    for role in ROSTER:
        assert role.delegates_to, role.name


def test_every_named_delegate_actually_exists() -> None:
    """A delegate that does not resolve is a promise to code that is not there,
    and it would read as 'already integrated' to the next person."""
    for role in ROSTER:
        for target in role.delegates_to:
            module = "aether." + target
            importlib.import_module(module)  # raises if absent


def test_a_role_without_a_delegate_is_refused() -> None:
    with pytest.raises(RosterError, match="no delegate named"):
        Role("rogue", None, frozenset({"CAP_AGENT"}), (), "does its own thinking")


def test_a_role_without_capabilities_is_refused() -> None:
    with pytest.raises(RosterError, match="can do nothing"):
        Role("mute", None, frozenset(), ("agents.population",), "")


# ==========================================================================
# spawnability — honest about what the kernel cannot back
# ==========================================================================
def test_roles_needing_unwired_capabilities_are_not_spawnable() -> None:
    for role in ROSTER:
        expected = not (role.capabilities & UNWIRED_CAPS)
        assert role.spawnable is expected, role.name


def test_exactly_the_expected_roles_are_unspawnable() -> None:
    """Named explicitly so quietly wiring or unwiring a capability shows up as
    a test change rather than a silent behaviour change."""
    unspawnable = sorted(r.name for r in ROSTER if not r.spawnable)
    assert unspawnable == [
        "ai_scientist_agent", "ocr_agent", "research_agent", "shell_agent",
        "vision_agent",
    ]


def test_invoking_an_unspawnable_role_raises_before_the_handler_runs() -> None:
    agents = build_roster()
    shell = agents["shell_agent"]
    ran = []
    shell.bind("run", lambda: ran.append(1), "CAP_EXEC")
    with pytest.raises(NotSpawnable, match="wires to nothing"):
        shell.invoke("run")
    assert not ran, "an unspawnable role must not reach its handler"


# ==========================================================================
# the capability gate
# ==========================================================================
def test_gate_raises_before_touching_the_subsystem() -> None:
    """A partially-executed refused action is worse than a refused one: it has
    already had effects nobody authorised."""
    agent = RoleAgent(ROSTER[0], frozenset({"CAP_AGENT"}))  # no CAP_MEMORY
    touched = []
    agent.bind("remember", lambda: touched.append(1), "CAP_MEMORY")
    with pytest.raises(CapabilityRefused, match="CAP_MEMORY"):
        agent.invoke("remember")
    assert not touched


def test_grant_can_be_smaller_than_the_declaration() -> None:
    """The declaration is a claim; the grant is the fact."""
    healer = next(r for r in ROSTER if r.name == "healer_agent")
    agent = RoleAgent(healer, frozenset({"CAP_AGENT"}))
    assert "CAP_REWRITE" in healer.capabilities
    assert "CAP_REWRITE" not in agent.effective


def test_grant_cannot_exceed_the_declaration() -> None:
    """Nobody can hand an agent more than its role declares."""
    agent = RoleAgent(ROSTER[0], frozenset({"CAP_AGENT", "CAP_SOVEREIGN"}))
    assert "CAP_SOVEREIGN" not in agent.effective


def test_refusals_are_recorded_for_audit() -> None:
    agent = RoleAgent(ROSTER[0], frozenset({"CAP_AGENT"}))
    with pytest.raises(CapabilityRefused):
        agent.require("CAP_MEMORY")
    assert len(agent.refusals) == 1 and "CAP_MEMORY" in agent.refusals[0]


def test_capability_refusal_is_its_own_type() -> None:
    """So refusals are countable and distinguishable from bugs."""
    assert issubclass(CapabilityRefused, RosterError)
    assert CapabilityRefused is not RosterError


def test_a_permitted_action_runs() -> None:
    agents = build_roster()
    orch = agents["orchestrator_agent"]
    orch.bind("route", lambda task: f"routed:{task}", "CAP_AGENT")
    assert orch.invoke("route", "build") == "routed:build"


def test_unknown_action_raises_rather_than_no_op() -> None:
    """A silent default makes a typo look like a deliberate no-op."""
    agents = build_roster()
    with pytest.raises(RosterError, match="no action"):
        agents["orchestrator_agent"].invoke("nonexistent")


def test_double_binding_is_refused() -> None:
    agents = build_roster()
    orch = agents["orchestrator_agent"]
    orch.bind("route", lambda: None, "CAP_AGENT")
    with pytest.raises(RosterError, match="already bound"):
        orch.bind("route", lambda: None, "CAP_AGENT")


def test_agent_with_no_grant_is_refused() -> None:
    with pytest.raises(RosterError, match="no capabilities granted"):
        RoleAgent(ROSTER[0], frozenset())


# ==========================================================================
# build_roster
# ==========================================================================
def test_build_roster_defaults_to_declared_capabilities() -> None:
    agents = build_roster()
    assert len(agents) == 12
    for role in ROSTER:
        assert agents[role.name].effective == role.capabilities


def test_build_roster_honours_a_restricted_grant() -> None:
    agents = build_roster({"healer_agent": frozenset({"CAP_AGENT"})})
    assert agents["healer_agent"].effective == frozenset({"CAP_AGENT"})
    assert agents["file_agent"].effective == frozenset({"CAP_AGENT", "CAP_MEMORY"})


def test_every_role_has_a_summary() -> None:
    for role in ROSTER:
        assert role.summary.strip(), role.name
