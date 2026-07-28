"""I-02 — capability enforcement layer.

Sits above the B-01 lockbox and answers a different question. The lockbox asks
*"does this DDR hold this capability token?"*; this layer asks *"is this
**principal** — the running process or agent — allowed to perform this
operation on this resource?"*

They are not the same check and collapsing them loses the one that matters here.
A module can legitimately hold ``CAP_WORLD_MODEL_WRITE`` while the principal
currently executing it is an ordinary ``AGENT`` that must not mutate shared
world state. That is exactly the I-02 case: a ``CAP_AGENT`` process attempting a
world-model write without ``CAP_SOVEREIGN`` must get a clean
:class:`CapError` — never a silent success.

**Fail closed.** :meth:`CapabilityEnforcer.check` denies any operation it has no
rule for. A default-allow enforcement layer is worse than none: every operation
added later ships unprotected, and the suite stays green while doing it, so the
gap is invisible until something uses it. Registering a rule is a deliberate
act; forgetting to must cost a failed call, not a silent grant.

Both outcomes are audited. Recording only denials makes it impossible to answer
"what did this principal actually do", which is the question that matters after
an incident.

This module deliberately does **not** import the Python-layer invariants or the
kernel's Section H set — the tiers here describe process authority and are
independent of both.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Any, Callable

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError

__all__ = [
    "Tier",
    "Principal",
    "OperationRule",
    "CapabilityEnforcer",
    "CapError",
    "EnforcementError",
    "CAP_AGENT",
    "CAP_SOVEREIGN",
]

DDR_ID = "I-02"
_FILE = "aether/capability/enforcement.py"

CAP_AGENT = "CAP_AGENT"
CAP_SOVEREIGN = "CAP_SOVEREIGN"

#: Denials surface as the lockbox's error type so callers handle one exception.
CapError = CapabilityError


class EnforcementError(ValueError):
    """Enforcement configuration refused."""


class Tier(IntEnum):
    """Process authority. Ordered so comparisons are unambiguous."""

    OBSERVER = 0        # may read, may not mutate anything
    AGENT = 1           # may mutate its own state
    SOVEREIGN = 2       # may mutate shared state


@dataclass(slots=True)
class Principal:
    """A running process or agent, and the authority it carries."""

    principal_id: str
    tier: Tier = Tier.AGENT
    capabilities: frozenset[str] = field(default_factory=frozenset)

    def holds(self, capability: str) -> bool:
        return capability in self.capabilities

    @property
    def sovereign(self) -> bool:
        return self.tier is Tier.SOVEREIGN and self.holds(CAP_SOVEREIGN)


@dataclass(slots=True)
class OperationRule:
    """What an operation demands of a principal."""

    operation: str
    min_tier: Tier
    required_capabilities: frozenset[str] = field(default_factory=frozenset)
    description: str = ""


class CapabilityEnforcer:
    """Fail-closed authority check for principal-level operations."""

    def __init__(
        self,
        audit: AuditLog | None = None,
        clock: Callable[[], float] | None = None,
    ) -> None:
        self.audit = audit if audit is not None else AuditLog()
        self.clock = clock if clock is not None else time.time
        self._rules: dict[str, OperationRule] = {}

    # -- configuration -------------------------------------------------------
    def register_rule(self, rule: OperationRule) -> OperationRule:
        if not rule.operation:
            raise EnforcementError("a rule needs an operation name")
        self._rules[rule.operation] = rule
        return rule

    def register_defaults(self) -> None:
        """The wired-in rules for the shipped mutating operations.

        Shared-state mutations demand SOVEREIGN; per-agent state demands only
        AGENT. Anything not listed is denied by :meth:`check`.
        """
        for rule in (
            OperationRule("world_model.write", Tier.SOVEREIGN,
                          frozenset({CAP_SOVEREIGN}),
                          "D-04 shared world state is not per-agent state"),
            OperationRule("knowledge.consolidate", Tier.SOVEREIGN,
                          frozenset({CAP_SOVEREIGN}),
                          "D-11 picks one winner for every consumer"),
            OperationRule("failure.record", Tier.AGENT, frozenset(),
                          "D-13 any agent may record its own failure"),
            OperationRule("hypothesis.generate", Tier.AGENT, frozenset(),
                          "D-07 proposing is not acting"),
            OperationRule("skill.approve", Tier.SOVEREIGN,
                          frozenset({CAP_SOVEREIGN}),
                          "D-15 approval is a second-party act"),
            OperationRule("world_model.read", Tier.OBSERVER, frozenset(),
                          "reads are unrestricted"),
        ):
            self.register_rule(rule)

    def rules(self) -> list[str]:
        return sorted(self._rules)

    # -- the check -----------------------------------------------------------
    def check(
        self, principal: Principal, operation: str, resource: str = ""
    ) -> bool:
        """Authorise ``principal`` for ``operation``, or raise.

        Returns True on success so it reads naturally at a call site; the
        failure path always raises rather than returning False, because a
        boolean return invites an unchecked call.
        """
        rule = self._rules.get(operation)
        if rule is None:
            # Fail closed. An unregistered operation is an operation nobody
            # decided about, and guessing "allow" makes every future operation
            # ship unprotected without anyone noticing.
            self._deny(principal, operation, resource,
                       f"no rule registered for {operation!r}")

        assert rule is not None      # _deny never returns
        if principal.tier < rule.min_tier:
            self._deny(
                principal, operation, resource,
                f"tier {principal.tier.name} below required "
                f"{rule.min_tier.name}",
            )
        missing = sorted(rule.required_capabilities - principal.capabilities)
        if missing:
            self._deny(principal, operation, resource,
                       f"missing capabilities {missing}")

        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="capability.allow",
            gate_status="allowed", principal=principal.principal_id,
            operation=operation, resource=resource,
            tier=principal.tier.name,
        )
        return True

    def _deny(
        self, principal: Principal, operation: str, resource: str, why: str
    ) -> None:
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="capability.deny",
            gate_status="denied", principal=principal.principal_id,
            operation=operation, resource=resource,
            tier=principal.tier.name, reason=why,
        )
        raise CapError(
            f"{principal.principal_id!r} may not perform {operation!r}"
            f"{f' on {resource!r}' if resource else ''}: {why}"
        )

    def allowed(self, principal: Principal, operation: str) -> bool:
        """Non-raising probe, for callers deciding what to offer.

        Deliberately separate from :meth:`check` — a probe must never be
        mistaken for enforcement.
        """
        try:
            self.check(principal, operation)
        except CapError:
            return False
        return True

    def guard(
        self, principal: Principal, operation: str, resource: str = ""
    ) -> Callable[[Callable[..., Any]], Callable[..., Any]]:
        """Decorator form: authorise before the wrapped call runs."""
        def _wrap(fn: Callable[..., Any]) -> Callable[..., Any]:
            def _guarded(*args: Any, **kwargs: Any) -> Any:
                self.check(principal, operation, resource)
                return fn(*args, **kwargs)
            _guarded.__name__ = getattr(fn, "__name__", "guarded")
            return _guarded
        return _wrap
