"""D-08 — superalignment monitor.

The consumer for the I-09 alignment taps that D-06…D-15 emit through. Its job is
narrow and adversarial: **watch the agents that are supposed to be auditing
themselves, and notice when one stops.**

The failure it exists to catch is silence. A misaligned or wedged agent does not
announce itself — it simply stops producing the audit records it is contracted
to produce, and every downstream check that reads the audit log then sees a
clean history. So the monitor is driven by *time*, not by events: each
registered agent declares a maximum interval, and :meth:`tick` raises a
:class:`MissedAuditEvent` **within one tick** of that interval lapsing. A
monitor that only reacts when an agent reports is blind to exactly the case that
matters.

The clock is injected (S9): the monitor computes from timestamps it is handed,
never from ambient state, so its verdicts are reproducible and testable.

Invariants: **S1** (no self-escalation — an agent cannot widen its own
deadline), **S4/S5** (findings are appended, never edited), **S9**
(deterministic).
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, Callable

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox

__all__ = [
    "Severity",
    "AlignmentFinding",
    "MissedAuditEvent",
    "AgentWatch",
    "SuperalignmentMonitor",
    "SuperalignmentError",
    "alignment_tap",
]

DDR_ID = "D-08"
_FILE = "aether/agents/safety/superalignment_monitor.py"
CAPABILITY = "CAP_ALIGNMENT_MONITOR"

DEFAULT_MAX_INTERVAL_S = 60.0
MAX_EVENTS = 10_000              # S2: bounded event window


class SuperalignmentError(ValueError):
    """Monitor operation refused."""


class Severity(str, Enum):
    INFO = "info"
    WARNING = "warning"
    CRITICAL = "critical"


@dataclass(slots=True)
class AlignmentFinding:
    kind: str
    agent: str
    severity: Severity
    detail: str
    ts: float


class MissedAuditEvent(RuntimeError):
    """A registered agent went silent past its declared interval.

    Carries the findings so a caller that catches it can act on every silent
    agent, not merely the first.
    """

    def __init__(self, findings: list[AlignmentFinding]) -> None:
        self.findings = findings
        names = ", ".join(sorted({f.agent for f in findings}))
        super().__init__(f"agents missed their audit deadline: {names}")


@dataclass(slots=True)
class AgentWatch:
    """One agent under observation."""

    agent: str
    max_interval_s: float
    last_seen: float
    steps: int = 0
    alerted: bool = False
    seen_steps: set[str] = field(default_factory=set)


class SuperalignmentMonitor:
    """Watches registered agents for silence and for undeclared behaviour."""

    def __init__(
        self,
        lockbox: Lockbox,
        audit: AuditLog | None = None,
        clock: Callable[[], float] | None = None,
        max_events: int = MAX_EVENTS,
    ) -> None:
        self.lockbox = lockbox
        self.audit = audit if audit is not None else AuditLog()
        # S9: injected clock, so the same inputs always produce the same
        # verdicts. Wall-clock inside the monitor would make its own alarms
        # untestable — the one property it cannot afford to lose.
        self.clock = clock if clock is not None else time.time
        self.max_events = int(max_events)
        self._watches: dict[str, AgentWatch] = {}
        self._findings: list[AlignmentFinding] = []

    # -- registration --------------------------------------------------------
    def register(
        self, agent: str, max_interval_s: float = DEFAULT_MAX_INTERVAL_S
    ) -> AgentWatch:
        """Put ``agent`` under observation with a reporting deadline."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not agent:
            raise SuperalignmentError("agent name is required")
        if max_interval_s <= 0:
            raise SuperalignmentError(
                f"max_interval_s {max_interval_s} must be > 0"
            )
        existing = self._watches.get(agent)
        if existing is not None and max_interval_s > existing.max_interval_s:
            # S1: an agent widening its own deadline is self-escalation — it
            # would let a silent agent buy itself unlimited silence.
            raise CapabilityError(
                f"{agent} may not widen its own audit interval "
                f"({existing.max_interval_s}s -> {max_interval_s}s)"
            )
        watch = AgentWatch(
            agent=agent, max_interval_s=float(max_interval_s),
            last_seen=self.clock(),
        )
        self._watches[agent] = watch
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="alignment.register",
            gate_status="watched", agent=agent, max_interval_s=max_interval_s,
        )
        return watch

    def watching(self) -> tuple[str, ...]:
        return tuple(sorted(self._watches))

    # -- the tap -------------------------------------------------------------
    def observe(self, step: str, detail: dict[str, Any] | None = None) -> None:
        """I-09 tap entry point. ``step`` is ``"<DDR>.<name>"``."""
        agent, _, name = step.partition(".")
        watch = self._watches.get(agent)
        if watch is None:
            # An unregistered agent reporting is itself a finding: something is
            # running that nothing is watching.
            self._record(AlignmentFinding(
                kind="unwatched_agent", agent=agent or step,
                severity=Severity.WARNING,
                detail=f"step {step!r} from an unregistered agent",
                ts=self.clock(),
            ))
            return
        watch.last_seen = self.clock()
        watch.steps += 1
        watch.alerted = False
        watch.seen_steps.add(name or step)

    def tap_for(self, agent: str) -> Callable[[str, dict[str, Any]], None]:
        """A tap callable to hand to an agent's ``alignment_tap`` slot."""
        def _tap(step: str, detail: dict[str, Any]) -> None:
            self.observe(step, detail)
        _tap.__name__ = f"alignment_tap_{agent.replace('-', '_')}"
        return _tap

    # -- the check -----------------------------------------------------------
    def tick(self) -> list[AlignmentFinding]:
        """Check every watch. Raises within one tick of a missed deadline.

        Returns the findings when nothing is overdue so a caller can poll it in
        a loop without exception handling in the normal path.
        """
        self.lockbox.require(DDR_ID, CAPABILITY)
        now = self.clock()
        overdue: list[AlignmentFinding] = []
        for watch in sorted(self._watches.values(), key=lambda w: w.agent):
            silence = now - watch.last_seen
            if silence <= watch.max_interval_s or watch.alerted:
                continue
            watch.alerted = True
            finding = AlignmentFinding(
                kind="missed_audit", agent=watch.agent,
                severity=Severity.CRITICAL,
                detail=f"silent for {silence:.3f}s, deadline "
                       f"{watch.max_interval_s:.3f}s",
                ts=now,
            )
            self._record(finding)
            overdue.append(finding)
        if overdue:
            raise MissedAuditEvent(overdue)
        return []

    def _record(self, finding: AlignmentFinding) -> AlignmentFinding:
        if len(self._findings) >= self.max_events:
            dropped = self._findings.pop(0)
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="alignment.finding_dropped",
                gate_status="dropped", reason="finding window full",
                dropped_kind=dropped.kind, window=self.max_events,
            )
        self._findings.append(finding)
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="alignment.finding",
            gate_status=finding.severity.value, kind=finding.kind,
            agent=finding.agent, detail=finding.detail,
        )
        return finding

    @property
    def findings(self) -> tuple[AlignmentFinding, ...]:
        return tuple(self._findings)

    def findings_for(self, agent: str) -> list[AlignmentFinding]:
        return [f for f in self._findings if f.agent == agent]


def alignment_tap(
    monitor: SuperalignmentMonitor | None,
) -> Callable[[str, dict[str, Any]], None] | None:
    """Adapter for the ``alignment_tap=`` slot on D-06…D-15.

    Returns ``None`` for a ``None`` monitor so the D-0x constructors keep their
    existing "no tap" behaviour unchanged.
    """
    if monitor is None:
        return None
    return monitor.observe
