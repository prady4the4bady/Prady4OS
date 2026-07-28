"""I-10 gate — the daemon coordinator, and the sole-spawn-path rule."""

from __future__ import annotations

import asyncio
from pathlib import Path

import pytest

from aether.agents.runtime.agent_bus import AgentBus
from aether.capability.enforcement import (
    CAP_AGENT,
    CAP_SOVEREIGN,
    CapabilityEnforcer,
    Principal,
    Tier,
)
from aether.daemon.coordinator import (
    SPAWN_OPERATION,
    AetherDaemon,
    AgentState,
    DaemonError,
    SpawnDenied,
)
from aether.kernel.audit.audit_log import AuditLog

_SOVEREIGN = Principal("daemon-owner", Tier.SOVEREIGN,
                       frozenset({CAP_AGENT, CAP_SOVEREIGN}))
_AGENT = Principal("worker-1", Tier.AGENT, frozenset({CAP_AGENT}))


def _daemon(tmp_path: Path) -> tuple[AetherDaemon, AgentBus, AuditLog]:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    bus = AgentBus(audit=audit)
    enf = CapabilityEnforcer(audit=audit)
    enf.register_defaults()
    return AetherDaemon(bus=bus, enforcer=enf, audit=audit), bus, audit


def test_spawned_agent_is_on_the_bus_within_one_tick(tmp_path: Path) -> None:
    """THE discriminator (half one).

    Registration and bus attachment must happen in the same call. An agent that
    exists but is not yet reachable silently drops everything addressed to it,
    and that window only opens under load.
    """
    daemon, bus, _audit = _daemon(tmp_path)
    record = daemon.spawn(_SOVEREIGN, "KRYOS")
    assert record.state is AgentState.RUNNING
    assert daemon.on_bus("KRYOS") is True          # already, with no sweep
    assert "KRYOS" in bus.agent_ids
    assert record.handle.agent_id == "KRYOS"


def test_side_channel_registration_is_detected(tmp_path: Path) -> None:
    """THE discriminator (half two) — S1, the daemon is the only spawn path.

    A side channel is the natural shape of the bug: some module calls
    bus.register() directly because it is right there. The agent works fine —
    and it is one the daemon never authorised, never capability-checked, and
    never told D-08 about, so nothing notices when it goes quiet.
    """
    daemon, bus, _audit = _daemon(tmp_path)
    daemon.spawn(_SOVEREIGN, "KRYOS")
    daemon.assert_sole_spawn_path()                # clean so far

    bus.register("SMUGGLED")                       # behind the daemon's back
    with pytest.raises(DaemonError, match="never spawned"):
        daemon.assert_sole_spawn_path()


def test_unauthorised_agent_detection_is_audited(tmp_path: Path) -> None:
    daemon, bus, _audit = _daemon(tmp_path)
    bus.register("SMUGGLED")
    with pytest.raises(DaemonError):
        daemon.assert_sole_spawn_path()
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["action"] == "daemon.unauthorised_agents"
               and e["extra"]["agents"] == ["SMUGGLED"] for e in entries)


def test_non_sovereign_principal_cannot_spawn(tmp_path: Path) -> None:
    """Spawning is capability-checked through the same I-02 layer as every
    other authority question."""
    daemon, bus, _audit = _daemon(tmp_path)
    with pytest.raises(SpawnDenied):
        daemon.spawn(_AGENT, "KRYOS")
    assert daemon.running() == []
    assert "KRYOS" not in bus.agent_ids            # nothing reached the bus


def test_spawn_denial_is_audited(tmp_path: Path) -> None:
    daemon, _bus, _audit = _daemon(tmp_path)
    with pytest.raises(SpawnDenied):
        daemon.spawn(_AGENT, "KRYOS")
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "I-10" and e["action"] == "daemon.spawn_denied"
               for e in entries)


def test_daemon_registers_its_own_spawn_rule(tmp_path: Path) -> None:
    """Fail-closed enforcement means an unregistered operation denies
    everything. The daemon owning its rule beats a caller papering over the
    denial with a permissive one."""
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    enf = CapabilityEnforcer(audit=audit)          # no defaults registered
    daemon = AetherDaemon(bus=AgentBus(audit=audit), enforcer=enf, audit=audit)
    assert SPAWN_OPERATION in enf.rules()
    assert daemon.spawn(_SOVEREIGN, "KRYOS").state is AgentState.RUNNING


def test_duplicate_spawn_refused(tmp_path: Path) -> None:
    daemon, _bus, _audit = _daemon(tmp_path)
    daemon.spawn(_SOVEREIGN, "KRYOS")
    with pytest.raises(DaemonError, match="already running"):
        daemon.spawn(_SOVEREIGN, "KRYOS")


def test_stop_then_respawn_is_allowed(tmp_path: Path) -> None:
    daemon, _bus, _audit = _daemon(tmp_path)
    daemon.spawn(_SOVEREIGN, "KRYOS")
    stopped = daemon.stop(_SOVEREIGN, "KRYOS")
    assert stopped.state is AgentState.STOPPED
    assert daemon.running() == []
    assert daemon.spawn(_SOVEREIGN, "KRYOS").state is AgentState.RUNNING


def test_stop_requires_authority_and_a_known_agent(tmp_path: Path) -> None:
    daemon, _bus, _audit = _daemon(tmp_path)
    daemon.spawn(_SOVEREIGN, "KRYOS")
    with pytest.raises(DaemonError, match="unknown agent"):
        daemon.stop(_SOVEREIGN, "GHOST")
    from aether.capability.enforcement import CapError
    with pytest.raises(CapError):
        daemon.stop(_AGENT, "KRYOS")


def test_empty_agent_id_refused(tmp_path: Path) -> None:
    daemon, _bus, _audit = _daemon(tmp_path)
    with pytest.raises(DaemonError, match="needs an id"):
        daemon.spawn(_SOVEREIGN, "")


def test_spawned_agent_can_actually_publish(tmp_path: Path) -> None:
    """'On the bus' has to mean usable, not merely listed."""
    daemon, bus, _audit = _daemon(tmp_path)
    record = daemon.spawn(_SOVEREIGN, "KRYOS")
    seen: list[dict] = []

    async def scenario() -> None:
        async with bus:
            record.handle.subscribe("telemetry", lambda m: seen.append(m.payload))
            await record.handle.publish("telemetry", {"cpu": 12})
            await bus.drain()

    asyncio.run(scenario())
    assert seen == [{"cpu": 12}]


def test_running_and_agents_views(tmp_path: Path) -> None:
    daemon, _bus, _audit = _daemon(tmp_path)
    daemon.spawn(_SOVEREIGN, "B")
    daemon.spawn(_SOVEREIGN, "A")
    daemon.stop(_SOVEREIGN, "B")
    assert daemon.running() == ["A"]
    assert [r.agent_id for r in daemon.agents] == ["A", "B"]
    assert daemon.get("A") is not None
    assert daemon.get("nope") is None


def test_alignment_tap_receives_steps(tmp_path: Path) -> None:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    events: list[str] = []
    enf = CapabilityEnforcer(audit=audit)
    enf.register_defaults()
    daemon = AetherDaemon(
        bus=AgentBus(audit=audit), enforcer=enf, audit=audit,
        alignment_tap=lambda s, _d: events.append(s),
    )
    daemon.spawn(_SOVEREIGN, "KRYOS")
    daemon.stop(_SOVEREIGN, "KRYOS")
    with pytest.raises(SpawnDenied):
        daemon.spawn(_AGENT, "OTHER")
    assert {"I-10.spawn", "I-10.stop", "I-10.spawn_denied"} <= set(events)
