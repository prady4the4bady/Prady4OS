"""I-09 gate — the real D-08 wiring replaces the stub taps across D-06…D-15."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from aether.agents.safety.alignment_wiring import TAPPED_AGENTS, taps_for, wire
from aether.agents.safety.superalignment_monitor import (
    MissedAuditEvent,
    SuperalignmentMonitor,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox


class _Clock:
    def __init__(self) -> None:
        self.now = 1000.0

    def __call__(self) -> float:
        return self.now

    def advance(self, dt: float) -> None:
        self.now += dt


def _monitor(tmp_path: Path):
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    lb.register("D-08", "superalignment_monitor.py", "CAP_ALIGNMENT_MONITOR")
    clock = _Clock()
    return SuperalignmentMonitor(lockbox=lb, audit=audit, clock=clock), clock, lb, audit


def test_an_agent_that_never_starts_is_still_caught(tmp_path: Path) -> None:
    """THE discriminator.

    Registering an agent the first time it reports is the natural-looking
    design and is exactly wrong: an agent that never starts never reports, so
    it is never registered, so the monitor never notices it is missing —
    invisible for precisely the agents that failed hardest.
    """
    mon, clock, _lb, _audit = _monitor(tmp_path)
    taps = wire(mon)

    # D-06 is alive and reporting; nothing else ever starts.
    taps["D-06"]("D-06.observe", {})
    clock.advance(30.0)
    assert mon.tick() == []                     # inside every deadline

    clock.advance(40.0)                         # 70s: past D-06 and D-12 (60s)
    with pytest.raises(MissedAuditEvent) as excinfo:
        mon.tick()
    silent = {f.agent for f in excinfo.value.findings}
    assert "D-12" in silent, "an agent that never started was not caught"


def test_every_tapped_agent_is_registered_up_front(tmp_path: Path) -> None:
    mon, _clock, _lb, _audit = _monitor(tmp_path)
    wire(mon)
    assert set(mon.watching()) == set(TAPPED_AGENTS)


def test_roster_covers_d06_through_d15(tmp_path: Path) -> None:
    """The slice promised the stub is replaced across D-06…D-15. D-08 is the
    monitor itself and does not tap itself."""
    expected = {f"D-{n:02d}" for n in range(6, 16)} - {"D-08"}
    assert set(TAPPED_AGENTS) == expected


def test_intervals_are_positive_and_per_agent(tmp_path: Path) -> None:
    """A single global interval is either too tight for the slow agents or too
    loose to catch the fast ones going quiet."""
    assert all(v > 0 for v in TAPPED_AGENTS.values())
    assert len(set(TAPPED_AGENTS.values())) > 1


def test_taps_route_by_ddr_prefix(tmp_path: Path) -> None:
    """An agent need not know which monitor it talks to — routing is on the
    'DDR.step' prefix the agents already emit."""
    mon, _clock, _lb, _audit = _monitor(tmp_path)
    taps = wire(mon)
    taps["D-11"]("D-11.consolidate", {})
    watch = mon._watches["D-11"]
    assert watch.steps == 1
    assert "consolidate" in watch.seen_steps


def test_taps_for_does_not_register(tmp_path: Path) -> None:
    """Separating the two lets a caller re-derive taps without re-registering
    (which would reset every last_seen and mask an in-flight silence)."""
    mon, _clock, _lb, _audit = _monitor(tmp_path)
    taps = taps_for(mon)
    assert mon.watching() == ()
    assert set(taps) == set(TAPPED_AGENTS)


def test_real_agents_wire_end_to_end(tmp_path: Path) -> None:
    """D-07 and D-15 constructed with the real taps must feed the monitor and
    keep it quiet — the stub-to-real swap with no agent-side change."""
    from aether.agents.research.hypothesis_generator import HypothesisGenerator
    from aether.agents.tool_composer.autonomous_skill_composer import (
        AutonomousSkillComposer,
        Skill,
    )

    mon, clock, lb, audit = _monitor(tmp_path)
    lb.register("D-07", "hypothesis_generator.py", "CAP_HYPOTHESIS_WRITE")
    lb.register("D-15", "autonomous_skill_composer.py", "CAP_ONTOLOGY_WRITE")
    taps = wire(mon)

    gen = HypothesisGenerator(
        lockbox=lb,
        generate_fn=lambda _c, _d: [{
            "statement": "s", "falsification_condition": "f",
            "expected_evidence": "e", "estimated_cost": 1.0,
        }],
        store_path=tmp_path / "h.jsonl", audit=audit,
        alignment_tap=taps["D-07"],
    )
    comp = AutonomousSkillComposer(
        lockbox=lb, agent_id="agent", store_path=tmp_path / "s.jsonl",
        audit=audit, alignment_tap=taps["D-15"],
    )
    gen.generate("ctx")
    comp.register_skill(Skill("x", "core"))

    assert mon._watches["D-07"].steps >= 1
    assert mon._watches["D-15"].steps >= 1
    # Neither is overdue at +30s; both intervals exceed that.
    clock.advance(30.0)
    findings = [f.agent for f in mon.findings]
    assert "D-07" not in findings and "D-15" not in findings


def test_no_unwatched_agent_findings_when_wired(tmp_path: Path) -> None:
    """A wired agent must never trip the 'unregistered agent reporting' path —
    that would fire on every normal step and bury the real alarms."""
    mon, _clock, _lb, _audit = _monitor(tmp_path)
    taps = wire(mon)
    for agent in TAPPED_AGENTS:
        taps[agent](f"{agent}.step", {})
    assert mon.findings == ()


def test_wire_is_idempotent(tmp_path: Path) -> None:
    """Re-wiring must not raise — the intervals are unchanged, so S1's
    no-widening rule permits it."""
    mon, _clock, _lb, _audit = _monitor(tmp_path)
    wire(mon)
    wire(mon)
    assert set(mon.watching()) == set(TAPPED_AGENTS)
