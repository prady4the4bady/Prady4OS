"""B-17 discriminating gate — blind-spot discovery loop."""

from __future__ import annotations

from pathlib import Path

from aether.agents.blindspot.discovery_loop import (
    BlindSpotKind,
    BlindSpotLoop,
    DEFAULT_STALE_AFTER_S,
)
from aether.agents.self_model.self_model import MIN_OBSERVATIONS, SelfModel
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox


def _setup(tmp_path: Path) -> tuple[Lockbox, SelfModel, AuditLog]:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    sm = SelfModel(lockbox=lb, store_path=tmp_path / "self_model.jsonl", audit=audit)
    return lb, sm, audit


def test_unclaimed_grant_is_found(tmp_path: Path) -> None:
    """The discriminator.

    A capability the lockbox grants but the self-model never claimed is a
    capability the system HOLDS and does not know it holds. A loop that only
    reads the self-model cannot see it — absence of a record is invisible from
    inside that record set.
    """
    lb, sm, audit = _setup(tmp_path)
    lb.register("D-10", "watchdog.py", "CAP_WATCHDOG")   # granted, never claimed
    loop = BlindSpotLoop(self_model=sm, audit=audit)

    spots = loop.discover()
    unclaimed = [s for s in spots if s.kind is BlindSpotKind.UNCLAIMED]
    assert len(unclaimed) == 1
    assert unclaimed[0].subject == "D-10:CAP_WATCHDOG"
    assert unclaimed[0].severity == 4


def test_claimed_capability_is_not_unclaimed(tmp_path: Path) -> None:
    lb, sm, audit = _setup(tmp_path)
    lb.register("D-10", "watchdog.py", "CAP_WATCHDOG")
    sm.claim_capability("watch agents", "D-10", "CAP_WATCHDOG")
    loop = BlindSpotLoop(self_model=sm, audit=audit)
    assert [s for s in loop.discover() if s.kind is BlindSpotKind.UNCLAIMED] == []


def test_untested_claim_is_found(tmp_path: Path) -> None:
    lb, sm, audit = _setup(tmp_path)
    lb.register("C-01", "sync.py", "CAP_NETWORK_SYNC")
    sm.claim_capability("sync", "C-01", "CAP_NETWORK_SYNC")
    loop = BlindSpotLoop(self_model=sm, audit=audit)
    untested = [s for s in loop.discover() if s.kind is BlindSpotKind.UNTESTED]
    assert len(untested) == 1
    assert "never exercised" in untested[0].reason


def test_thinly_observed_claim_is_still_a_blind_spot(tmp_path: Path) -> None:
    lb, sm, audit = _setup(tmp_path)
    lb.register("C-01", "sync.py", "CAP_NETWORK_SYNC")
    sm.claim_capability("sync", "C-01", "CAP_NETWORK_SYNC")
    sm.record_outcome("sync", success=True)          # 1 observation < MIN
    loop = BlindSpotLoop(self_model=sm, audit=audit)
    untested = [s for s in loop.discover() if s.kind is BlindSpotKind.UNTESTED]
    assert untested and "observation" in untested[0].reason


def test_stale_evidence_is_found_with_an_injected_clock(tmp_path: Path) -> None:
    """A staleness rule that needs a 30-day wait to test is never tested."""
    lb, sm, audit = _setup(tmp_path)
    lb.register("C-01", "sync.py", "CAP_NETWORK_SYNC")
    sm.claim_capability("sync", "C-01", "CAP_NETWORK_SYNC")
    for _ in range(MIN_OBSERVATIONS):
        sm.record_outcome("sync", success=True)

    claim = sm.claims[0]
    future = claim.last_used_ts + DEFAULT_STALE_AFTER_S + 86400.0
    loop = BlindSpotLoop(self_model=sm, audit=audit, now_fn=lambda: future)
    stale = [s for s in loop.discover() if s.kind is BlindSpotKind.STALE]
    assert len(stale) == 1
    assert "days" in stale[0].reason


def test_fresh_evidence_is_not_stale(tmp_path: Path) -> None:
    lb, sm, audit = _setup(tmp_path)
    lb.register("C-01", "sync.py", "CAP_NETWORK_SYNC")
    sm.claim_capability("sync", "C-01", "CAP_NETWORK_SYNC")
    for _ in range(MIN_OBSERVATIONS):
        sm.record_outcome("sync", success=True)
    loop = BlindSpotLoop(self_model=sm, audit=audit)
    assert [s for s in loop.discover() if s.kind is BlindSpotKind.STALE] == []


def test_results_are_severity_ordered(tmp_path: Path) -> None:
    lb, sm, audit = _setup(tmp_path)
    lb.register("D-10", "watchdog.py", "CAP_WATCHDOG")       # unclaimed: sev 4
    lb.register("C-01", "sync.py", "CAP_NETWORK_SYNC")
    sm.claim_capability("sync", "C-01", "CAP_NETWORK_SYNC")  # untested: sev 3
    loop = BlindSpotLoop(self_model=sm, audit=audit)
    spots = loop.discover()
    assert spots[0].severity >= spots[-1].severity
    assert loop.worst(limit=1)[0].kind is BlindSpotKind.UNCLAIMED


def test_clean_system_has_no_blind_spots(tmp_path: Path) -> None:
    lb, sm, audit = _setup(tmp_path)
    lb.register("C-01", "sync.py", "CAP_NETWORK_SYNC")
    sm.claim_capability("sync", "C-01", "CAP_NETWORK_SYNC")
    for _ in range(MIN_OBSERVATIONS):
        sm.record_outcome("sync", success=True)
    loop = BlindSpotLoop(self_model=sm, audit=audit)
    assert loop.discover() == []


def test_discovery_is_audited(tmp_path: Path) -> None:
    audit_path = tmp_path / "audit_log.jsonl"
    audit = AuditLog(audit_path)
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    sm = SelfModel(lockbox=lb, store_path=tmp_path / "sm.jsonl", audit=audit)
    BlindSpotLoop(self_model=sm, audit=audit).discover()
    entries = AuditLog(audit_path).read_all()
    assert any(e["ddr"] == "B-17" and e["action"] == "blindspot.discover" for e in entries)
