"""I-10 — the AETHER daemon coordinator.

The single process that brings agents up, puts them on the B-07 bus, and takes
them down.

**The daemon is the ONLY spawn path (S1).** Not by convention — by refusal.
:meth:`AetherDaemon.spawn` is the only method that registers an agent, and
:meth:`assert_sole_spawn_path` fails when anything else has put an agent on the
bus behind its back. That check exists because a side channel is the natural
shape of the bug: some module calls ``bus.register()`` directly because it is
right there, the agent works fine, and now there is a running agent the daemon
never authorised, never capability-checked, and — the part that matters — never
registered with the D-08 monitor, so its silence is invisible. Every control in
this layer keys off the daemon knowing what is running.

**A spawned agent is on the bus within one tick.** Registration and bus
attachment happen in the same call rather than being deferred to a later sweep.
An agent that exists but is not yet reachable is a window in which messages
addressed to it are dropped, and the window is exactly the kind that only opens
under load.

Spawning is capability-checked through the I-02 enforcer, so "who may start an
agent" is answered by the same layer that answers every other authority
question.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable

from aether.agents.runtime.agent_bus import AgentBus, AgentHandle
from aether.capability.enforcement import (
    CapabilityEnforcer,
    CapError,
    OperationRule,
    Principal,
    Tier,
)
from aether.kernel.audit.audit_log import AuditLog

__all__ = [
    "AgentState",
    "AgentRecord",
    "AetherDaemon",
    "DaemonError",
    "SpawnDenied",
    "SPAWN_OPERATION",
]

DDR_ID = "I-10"
_FILE = "aether/daemon/coordinator.py"

SPAWN_OPERATION = "daemon.spawn"


class DaemonError(ValueError):
    """Daemon operation refused."""


class SpawnDenied(CapError):
    """The principal may not spawn agents."""


class AgentState(str, Enum):
    SPAWNED = "spawned"
    RUNNING = "running"
    STOPPED = "stopped"


@dataclass(slots=True)
class AgentRecord:
    agent_id: str
    handle: AgentHandle = field(repr=False)
    state: AgentState = AgentState.SPAWNED
    spawned_by: str = ""
    spawned_ts: float = 0.0
    stopped_ts: float = 0.0


class AetherDaemon:
    """Brings agents up on the bus, and is the only thing allowed to."""

    def __init__(
        self,
        bus: AgentBus,
        enforcer: CapabilityEnforcer,
        audit: AuditLog | None = None,
        alignment_tap: Callable[[str, dict[str, Any]], None] | None = None,
        clock: Callable[[], float] | None = None,
    ) -> None:
        self.bus = bus
        self.enforcer = enforcer
        self.audit = audit if audit is not None else AuditLog()
        self.alignment_tap = alignment_tap
        self.clock = clock if clock is not None else time.time
        self._agents: dict[str, AgentRecord] = {}
        # Registering the rule here rather than expecting the caller to means
        # the daemon cannot be constructed into a state where spawning is
        # unguarded — fail-closed would deny everything, which is safe but
        # useless, and a caller papering over that with a permissive rule is
        # worse than the daemon owning its own.
        if SPAWN_OPERATION not in enforcer.rules():
            enforcer.register_rule(OperationRule(
                SPAWN_OPERATION, Tier.SOVEREIGN,
                frozenset({"CAP_SOVEREIGN"}),
                "I-10: only a sovereign principal may start an agent",
            ))

    def _emit(self, step: str, detail: dict[str, Any]) -> None:
        if self.alignment_tap is not None:
            self.alignment_tap(f"{DDR_ID}.{step}", detail)

    # -- spawn ---------------------------------------------------------------
    def spawn(self, principal: Principal, agent_id: str) -> AgentRecord:
        """Start an agent and attach it to the bus in the same call."""
        if not agent_id:
            raise DaemonError("an agent needs an id")
        if agent_id in self._agents and self._agents[agent_id].state is not AgentState.STOPPED:
            raise DaemonError(f"agent {agent_id!r} is already running")
        try:
            self.enforcer.check(principal, SPAWN_OPERATION, resource=agent_id)
        except CapError as exc:
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="daemon.spawn_denied",
                gate_status="denied", agent=agent_id,
                principal=principal.principal_id,
            )
            self._emit("spawn_denied", {"agent": agent_id})
            raise SpawnDenied(str(exc)) from exc

        # Bus attachment happens HERE, not in a later sweep: an agent that
        # exists but is unreachable silently drops everything addressed to it.
        handle = self.bus.register(agent_id)
        record = AgentRecord(
            agent_id=agent_id, handle=handle, state=AgentState.RUNNING,
            spawned_by=principal.principal_id, spawned_ts=self.clock(),
        )
        self._agents[agent_id] = record
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="daemon.spawn", gate_status="running",
            agent=agent_id, principal=principal.principal_id,
        )
        self._emit("spawn", {"agent": agent_id})
        return record

    def stop(self, principal: Principal, agent_id: str) -> AgentRecord:
        record = self._agents.get(agent_id)
        if record is None:
            raise DaemonError(f"unknown agent: {agent_id}")
        self.enforcer.check(principal, SPAWN_OPERATION, resource=agent_id)
        self.bus.revoke(agent_id)
        record.state = AgentState.STOPPED
        record.stopped_ts = self.clock()
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="daemon.stop", gate_status="stopped",
            agent=agent_id, principal=principal.principal_id,
        )
        self._emit("stop", {"agent": agent_id})
        return record

    # -- the S1 check --------------------------------------------------------
    def assert_sole_spawn_path(self) -> None:
        """Fail if anything reached the bus without going through the daemon.

        This is the enforcement, not the docstring above it. An agent registered
        by a side channel is one the daemon never authorised, never
        capability-checked, and never told D-08 about — so nothing notices when
        it goes quiet.
        """
        on_bus = set(self.bus.agent_ids)
        known = set(self._agents)
        unauthorised = sorted(on_bus - known)
        if unauthorised:
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="daemon.unauthorised_agents",
                gate_status="violation", agents=unauthorised,
            )
            self._emit("unauthorised_agents", {"agents": unauthorised})
            raise DaemonError(
                f"agents on the bus the daemon never spawned: {unauthorised} — "
                f"the daemon must be the only spawn path (S1)"
            )

    # -- reads ---------------------------------------------------------------
    def get(self, agent_id: str) -> AgentRecord | None:
        return self._agents.get(agent_id)

    def running(self) -> list[str]:
        return sorted(a for a, r in self._agents.items()
                      if r.state is AgentState.RUNNING)

    def on_bus(self, agent_id: str) -> bool:
        return agent_id in self.bus.agent_ids

    @property
    def agents(self) -> tuple[AgentRecord, ...]:
        return tuple(sorted(self._agents.values(), key=lambda r: r.agent_id))
