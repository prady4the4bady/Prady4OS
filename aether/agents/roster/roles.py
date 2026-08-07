"""Section G — the 12-agent roster, with real behaviour (DDR-857).

DDR-846 gave the eight legacy slots a `skill.md` each: a prompt saying what the
agent is. This gives them *logic* — what the agent actually does when asked.

THE DESIGN DECISION, and the reason this file is short: **an agent is a thin,
capability-gated dispatcher over subsystems that already exist.** It routes and
refuses; it does not reason.

That is a direct consequence of DDR-855, where I rebuilt D-07 and D-13 because I
had not looked first. The pull here is identical and stronger — every role reads
like it wants its own planner, its own memory, its own verifier. So the rule is
explicit: a role method either delegates to a named existing module or it
refuses. Where a subsystem is not yet wired, the role declares itself **not
spawnable** rather than shipping a private half-implementation that would drift
from the real one and win by being imported first.

The capability gate is the other half. An agent asked to do something outside
its declared capabilities raises `CapabilityRefused` **before** touching a
subsystem, because a partially-executed refused action is worse than a refused
one: it has already had effects nobody authorised.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable

__all__ = [
    "RosterError",
    "CapabilityRefused",
    "NotSpawnable",
    "Role",
    "RoleAgent",
    "ROSTER",
    "build_roster",
    "UNWIRED_CAPS",
]

# Declared in kernel/cap.h with no enforcement path behind them (DDR-846).
# A role needing one of these is defined but NOT spawnable.
UNWIRED_CAPS = frozenset({"CAP_EXEC", "CAP_OCR", "CAP_SCENE", "CAP_NET_BROWSE"})


class RosterError(ValueError):
    """A roster operation that must not proceed."""


class CapabilityRefused(RosterError):
    """The agent lacks a capability the action requires.

    Its own type so refusals are countable and distinguishable from bugs —
    the same reasoning that gave privacy blocking its own exception (DDR-852)
    and `AR_PRIVACY_BLOCKED` its own audit code.
    """


class NotSpawnable(RosterError):
    """The role is defined but its capabilities are not wired in the kernel."""


@dataclass(frozen=True)
class Role:
    """One roster role: what it is, what it may do, and what backs it.

    `delegates_to` is not documentation. It is the record of which existing
    subsystem does the work, and it is asserted by a test — a role that names
    nothing has, by this file's rule, no legitimate implementation.
    """

    name: str
    slot: str | None                 # legacy KRYOS…SOLIN slot, if any
    capabilities: frozenset[str]
    delegates_to: tuple[str, ...]
    summary: str

    def __post_init__(self) -> None:
        if not self.name.strip():
            raise RosterError("role requires a name")
        if not self.capabilities:
            raise RosterError(f"{self.name}: a role with no capabilities can do nothing")
        if not self.delegates_to:
            raise RosterError(
                f"{self.name}: no delegate named. A role that implements its own "
                "reasoning duplicates a subsystem and wins by being imported "
                "first — the DDR-855 failure"
            )

    @property
    def spawnable(self) -> bool:
        return not (self.capabilities & UNWIRED_CAPS)

    @property
    def blocking_caps(self) -> tuple[str, ...]:
        return tuple(sorted(self.capabilities & UNWIRED_CAPS))


#: The 12 roles. The first eight take the legacy kernel slots (DDR-846); the
#: last four are new and have no slot yet, which is itself a reason they are
#: not spawnable — a role with no roster slot cannot be scheduled.
ROSTER: tuple[Role, ...] = (
    Role("file_agent", "KRYOS", frozenset({"CAP_AGENT", "CAP_MEMORY"}),
         ("agents.memory.knowledge_consolidation",),
         "Reads, writes and deletes files on the operator's behalf."),
    Role("shell_agent", "PRAX", frozenset({"CAP_AGENT", "CAP_EXEC"}),
         ("agents.quarantine.namespace",),
         "Runs commands inside a quarantine namespace."),
    Role("research_agent", "LUMYN", frozenset({"CAP_AGENT", "CAP_NET_BROWSE"}),
         ("agents.research.hypothesis_generator", "agents.research.hypothesis_tree"),
         "Forms falsifiable hypotheses and gathers evidence."),
    Role("ocr_agent", "AHNIS", frozenset({"CAP_AGENT", "CAP_OCR"}),
         ("agents.routing.ocr_memory",),
         "Turns documents into memory records, confidence preserved."),
    Role("vision_agent", "IRIS", frozenset({"CAP_AGENT", "CAP_SCENE"}),
         ("agents.routing.ocr_memory",),
         "Describes scenes and answers questions about them."),
    Role("healer_agent", "RUFLO", frozenset({"CAP_AGENT", "CAP_REWRITE"}),
         ("agents.moss", "agents.introspection.failure_analysis"),
         "Diagnoses failures and proposes source repairs through MOSS."),
    Role("orchestrator_agent", "HERMES", frozenset({"CAP_AGENT"}),
         ("agents.runtime.agent_bus", "agents.planner.long_horizon_planner"),
         "Decomposes work and routes it to the other agents."),
    Role("verifier_agent", "SOLIN", frozenset({"CAP_AGENT"}),
         ("agents.research.experiment_outcome_evaluator", "agents.contracts.agent_contract"),
         "Checks claims against evidence before they are acted on."),
    Role("subconscious_agent", None, frozenset({"CAP_AGENT", "CAP_MEMORY"}),
         ("agents.goals.subconscious", "agents.world_model.world_model"),
         "Wakes on a period, diffs goals, proposes work to idle agents."),
    Role("ai_scientist_agent", None, frozenset({"CAP_AGENT", "CAP_EXEC"}),
         ("agents.research.hypothesis_generator", "agents.memory.failure_memory_registry"),
         "Designs and runs experiments against a hypothesis tree."),
    Role("architect_agent", None, frozenset({"CAP_AGENT"}),
         ("agents.planner.long_horizon_planner", "agents.capability.boundary_mapper"),
         "Plans multi-step changes and maps them onto capabilities."),
    Role("tournament_agent", None, frozenset({"CAP_AGENT"}),
         ("agents.population",),
         "Runs variants head-to-head and reports a champion, or none."),
)


@dataclass
class RoleAgent:
    """A live agent: a role, the capabilities the KERNEL granted, and a bus.

    `granted` is what the kernel gave, which may be *less* than the role
    declares. The gate checks the intersection, so a role that declares
    `CAP_REWRITE` but was granted only `CAP_AGENT` cannot rewrite anything —
    the declaration is a claim, the grant is the fact.
    """

    role: Role
    granted: frozenset[str]
    _handlers: dict[str, Callable[..., Any]] = field(default_factory=dict)
    refusals: list[str] = field(default_factory=list)

    def __post_init__(self) -> None:
        if not self.granted:
            raise RosterError(f"{self.role.name}: no capabilities granted")

    @property
    def effective(self) -> frozenset[str]:
        """What the agent can actually use: declared AND granted."""
        return self.role.capabilities & self.granted

    def require(self, *caps: str) -> None:
        """Gate an action. Raises BEFORE any subsystem is touched.

        Order matters: a partially-executed refused action is worse than a
        refused one, because it has already had effects nobody authorised.
        """
        missing = sorted(set(caps) - self.effective)
        if missing:
            reason = (
                f"{self.role.name} lacks {missing} "
                f"(declared {sorted(self.role.capabilities)}, "
                f"granted {sorted(self.granted)})"
            )
            self.refusals.append(reason)
            raise CapabilityRefused(reason)

    def ensure_spawnable(self) -> None:
        if not self.role.spawnable:
            raise NotSpawnable(
                f"{self.role.name} needs {list(self.role.blocking_caps)}, which "
                "kernel/cap.h declares but wires to nothing. Declaring it "
                "unspawnable is honest; shipping a private half-implementation "
                "would drift from the real subsystem and win by being imported "
                "first (DDR-855)"
            )

    def bind(self, action: str, handler: Callable[..., Any], *caps: str) -> None:
        """Attach a handler that delegates to a subsystem."""
        if action in self._handlers:
            raise RosterError(f"{self.role.name}: action {action!r} already bound")
        self._handlers[action] = (handler, tuple(caps))  # type: ignore[assignment]

    def invoke(self, action: str, *args: Any, **kwargs: Any) -> Any:
        """Run a bound action, capability-gated and spawnability-gated."""
        entry = self._handlers.get(action)
        if entry is None:
            raise RosterError(
                f"{self.role.name} has no action {action!r} — refusing rather "
                "than falling back to a default, which would make a typo look "
                "like a deliberate no-op"
            )
        handler, caps = entry  # type: ignore[misc]
        self.ensure_spawnable()
        if caps:
            self.require(*caps)
        return handler(*args, **kwargs)


def build_roster(
    grants: dict[str, frozenset[str]] | None = None,
) -> dict[str, RoleAgent]:
    """Instantiate every role. Grants default to what each role declares.

    Defaulting to the declared set means the common call site produces agents
    that can do exactly what their `skill.md` says — no more. A caller wanting
    a restricted agent passes less; nobody can pass more than the role declares,
    because `effective` intersects.
    """
    grants = grants or {}
    return {
        role.name: RoleAgent(role, grants.get(role.name, role.capabilities))
        for role in ROSTER
    }
