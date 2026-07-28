"""D-08 discriminating gate — superalignment monitor."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.agents.safety.superalignment_monitor import (
    AlignmentFinding,
    MissedAuditEvent,
    Severity,
    SuperalignmentError,
    SuperalignmentMonitor,
    alignment_tap,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


class _Clock:
    """Injected clock — the monitor must not read wall time (S9)."""

    def __init__(self) -> None:
        self.now = 1000.0

    def __call__(self) -> float:
        return self.now

    def advance(self, dt: float) -> None:
        self.now += dt


def _monitor(tmp_path: Path, granted: bool = True) -> tuple[SuperalignmentMonitor, _Clock]:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("D-08", "superalignment_monitor.py", "CAP_ALIGNMENT_MONITOR")
    clock = _Clock()
    return SuperalignmentMonitor(lockbox=lb, audit=audit, clock=clock), clock


def test_missed_audit_raises_within_one_tick(tmp_path: Path) -> None:
    """THE discriminator.

    A monitor driven by events cannot see silence — and silence is exactly how
    a wedged or misaligned agent presents. The deadline lapses at t+10; the very
    next tick must raise, not some later one.
    """
    mon, clock = _monitor(tmp_path)
    mon.register("D-06", max_interval_s=10.0)

    clock.advance(9.0)
    assert mon.tick() == []              # still inside the deadline

    clock.advance(1.5)                   # 10.5s of silence — past the deadline
    with pytest.raises(MissedAuditEvent) as excinfo:
        mon.tick()
    findings = excinfo.value.findings
    assert [f.agent for f in findings] == ["D-06"]
    assert findings[0].severity is Severity.CRITICAL
    assert findings[0].kind == "missed_audit"


def test_reporting_agent_never_alerts(tmp_path: Path) -> None:
    """An agent that keeps reporting must never be flagged, however long it runs."""
    mon, clock = _monitor(tmp_path)
    mon.register("D-06", max_interval_s=10.0)
    for _ in range(20):
        clock.advance(9.0)
        mon.observe("D-06.infer", {})
        assert mon.tick() == []
    assert mon.findings_for("D-06") == []


def test_alert_is_not_repeated_until_the_agent_recovers(tmp_path: Path) -> None:
    """One finding per silence episode — a repeating alarm is noise, and noise
    is how a real alert gets ignored."""
    mon, clock = _monitor(tmp_path)
    mon.register("D-06", max_interval_s=10.0)
    clock.advance(11.0)
    with pytest.raises(MissedAuditEvent):
        mon.tick()
    clock.advance(11.0)
    assert mon.tick() == []                     # already alerted
    assert len(mon.findings_for("D-06")) == 1

    mon.observe("D-06.infer", {})               # recovered
    clock.advance(11.0)
    with pytest.raises(MissedAuditEvent):       # a new episode alerts again
        mon.tick()
    assert len(mon.findings_for("D-06")) == 2


def test_every_silent_agent_is_reported_not_just_the_first(tmp_path: Path) -> None:
    mon, clock = _monitor(tmp_path)
    for name in ("D-06", "D-07", "D-09"):
        mon.register(name, max_interval_s=5.0)
    clock.advance(3.0)
    mon.observe("D-07.generate", {})            # only D-07 stays alive
    clock.advance(3.0)                          # D-07 silent 3s, others 6s
    with pytest.raises(MissedAuditEvent) as excinfo:
        mon.tick()
    assert sorted(f.agent for f in excinfo.value.findings) == ["D-06", "D-09"]


def test_agent_may_not_widen_its_own_deadline(tmp_path: Path) -> None:
    """S1 — self-escalation. Widening the interval buys unlimited silence."""
    mon, _clock = _monitor(tmp_path)
    mon.register("D-06", max_interval_s=10.0)
    with pytest.raises(CapabilityError, match="may not widen"):
        mon.register("D-06", max_interval_s=600.0)
    # Tightening is always allowed — it can only make the monitor stricter.
    assert mon.register("D-06", max_interval_s=2.0).max_interval_s == 2.0


def test_unregistered_agent_reporting_is_a_finding(tmp_path: Path) -> None:
    """Something running that nothing watches is itself the alarm."""
    mon, _clock = _monitor(tmp_path)
    mon.observe("D-99.rogue", {})
    findings = mon.findings_for("D-99")
    assert len(findings) == 1
    assert findings[0].kind == "unwatched_agent"
    assert findings[0].severity is Severity.WARNING


def test_tap_wires_a_real_agent(tmp_path: Path) -> None:
    """I-09 end to end: D-07's tap slot feeds this monitor unchanged."""
    from aether.agents.research.hypothesis_generator import HypothesisGenerator

    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    lb.register("D-08", "superalignment_monitor.py", "CAP_ALIGNMENT_MONITOR")
    lb.register("D-07", "hypothesis_generator.py", "CAP_HYPOTHESIS_WRITE")
    clock = _Clock()
    mon = SuperalignmentMonitor(lockbox=lb, audit=audit, clock=clock)
    mon.register("D-07", max_interval_s=10.0)

    gen = HypothesisGenerator(
        lockbox=lb,
        generate_fn=lambda _c, _d: [{
            "statement": "s", "falsification_condition": "f",
            "expected_evidence": "e", "estimated_cost": 1.0,
        }],
        store_path=tmp_path / "h.jsonl", audit=audit,
        alignment_tap=alignment_tap(mon),
    )
    gen.generate("ctx")
    clock.advance(9.0)
    assert mon.tick() == []                     # the agent reported through
    assert "generate" in mon._watches["D-07"].seen_steps

    clock.advance(2.0)                          # now it goes quiet
    with pytest.raises(MissedAuditEvent):
        mon.tick()


def test_alignment_tap_none_passthrough() -> None:
    """A None monitor must leave the D-0x 'no tap' behaviour untouched."""
    assert alignment_tap(None) is None


def test_bad_registration_refused(tmp_path: Path) -> None:
    mon, _clock = _monitor(tmp_path)
    with pytest.raises(SuperalignmentError, match="agent name is required"):
        mon.register("", 10.0)
    with pytest.raises(SuperalignmentError, match="must be > 0"):
        mon.register("D-06", 0.0)


def test_capability_required(tmp_path: Path) -> None:
    mon, _clock = _monitor(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_ALIGNMENT_MONITOR"):
        mon.register("D-06", 10.0)


def test_findings_are_audited(tmp_path: Path) -> None:
    """S4/S5 — findings are appended to the audit log, never edited."""
    mon, clock = _monitor(tmp_path)
    mon.register("D-06", max_interval_s=1.0)
    clock.advance(2.0)
    with pytest.raises(MissedAuditEvent):
        mon.tick()
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "D-08" and e["action"] == "alignment.finding"
               and e["extra"]["kind"] == "missed_audit" for e in entries)


def test_finding_window_is_bounded_and_drops_logged(tmp_path: Path) -> None:
    """S2 bounded + no silent loss."""
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    lb.register("D-08", "superalignment_monitor.py", "CAP_ALIGNMENT_MONITOR")
    mon = SuperalignmentMonitor(lockbox=lb, audit=audit, clock=_Clock(), max_events=3)
    for i in range(5):
        mon.observe(f"D-9{i}.rogue", {})
    assert len(mon.findings) == 3
    drops = [e for e in AuditLog(tmp_path / "audit_log.jsonl").read_all()
             if e["action"] == "alignment.finding_dropped"]
    assert len(drops) == 2
    assert drops[0]["extra"]["reason"]


def test_watching_lists_registrations(tmp_path: Path) -> None:
    mon, _clock = _monitor(tmp_path)
    mon.register("D-07", 5.0)
    mon.register("D-06", 5.0)
    assert mon.watching() == ("D-06", "D-07")


def test_finding_is_a_plain_record() -> None:
    f = AlignmentFinding("missed_audit", "D-06", Severity.CRITICAL, "d", 1.0)
    assert f.severity.value == "critical"
