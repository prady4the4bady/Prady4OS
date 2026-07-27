"""B-01 discriminating gate — CAP_SOVEREIGN lockbox."""

from __future__ import annotations

from pathlib import Path

import pytest

from kernel.audit.audit_log import AuditLog
from kernel.lockbox.cap_sovereign import (
    CAP_SOVEREIGN,
    KNOWN_CAPABILITIES,
    CapabilityError,
    Lockbox,
)


def _lockbox(tmp_path: Path) -> Lockbox:
    return Lockbox(
        registry_path=tmp_path / "tokens.jsonl",
        audit=AuditLog(tmp_path / "audit_log.jsonl"),
    )


def test_register_and_check(tmp_path: Path) -> None:
    lb = _lockbox(tmp_path)
    tok = lb.register("C-01", "agents/federation/knowledge_sync.py", "CAP_NETWORK_SYNC")
    assert tok.token
    assert lb.check("C-01", "CAP_NETWORK_SYNC") is True
    assert lb.check("C-01", "CAP_WATCHDOG") is False


def test_require_raises_without_capability(tmp_path: Path) -> None:
    lb = _lockbox(tmp_path)
    lb.register("C-01", "x.py", "CAP_NETWORK_SYNC")
    with pytest.raises(CapabilityError, match="S6"):
        lb.require("C-01", "CAP_WATCHDOG")


def test_self_escalation_to_sovereign_is_refused(tmp_path: Path) -> None:
    """S1 — nothing may mint CAP_SOVEREIGN for itself."""
    lb = _lockbox(tmp_path)
    with pytest.raises(CapabilityError, match="S1"):
        lb.register("EVIL", "evil.py", CAP_SOVEREIGN)


def test_sovereign_grant_requires_sovereign_approver(tmp_path: Path) -> None:
    lb = _lockbox(tmp_path)
    non_sovereign = lb.register("C-01", "x.py", "CAP_NETWORK_SYNC")
    with pytest.raises(CapabilityError, match="S1"):
        lb.register("D-08", "y.py", CAP_SOVEREIGN, approved_by=non_sovereign)


def test_unknown_capability_rejected(tmp_path: Path) -> None:
    lb = _lockbox(tmp_path)
    with pytest.raises(CapabilityError, match="unknown capability"):
        lb.register("C-99", "z.py", "CAP_MAKE_IT_UP")


def test_token_binds_file_and_mask(tmp_path: Path) -> None:
    """A token must not be replayable for a different file or a wider mask."""
    lb = _lockbox(tmp_path)
    t1 = lb.register("C-01", "a.py", "CAP_NETWORK_SYNC")
    t2 = lb.register("C-01b", "b.py", "CAP_NETWORK_SYNC")
    t3 = lb.register("C-01c", "a.py", "CAP_WATCHDOG")
    assert t1.token != t2.token          # different file
    assert t1.token != t3.token          # different mask


def test_registry_survives_restart(tmp_path: Path) -> None:
    reg = tmp_path / "tokens.jsonl"
    audit = tmp_path / "audit_log.jsonl"
    lb = Lockbox(registry_path=reg, audit=AuditLog(audit))
    lb.register("D-10", "agents/watchdog/watchdog.py", "CAP_WATCHDOG")
    reborn = Lockbox(registry_path=reg, audit=AuditLog(audit))
    assert reborn.check("D-10", "CAP_WATCHDOG") is True


def test_registration_is_audited(tmp_path: Path) -> None:
    audit_path = tmp_path / "audit_log.jsonl"
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=AuditLog(audit_path))
    lb.register("D-13", "agents/runtime/cognitive_load_balancer.py", "CAP_LOAD_BALANCE")
    entries = AuditLog(audit_path).read_all()
    assert any(e["action"] == "lockbox.register" and e["ddr"] == "D-13" for e in entries)


def test_register_is_idempotent(tmp_path: Path) -> None:
    lb = _lockbox(tmp_path)
    a = lb.register("C-04", "agents/invention/registry.py", "CAP_INVENTION_WRITE")
    b = lb.register("C-04", "agents/invention/registry.py", "CAP_INVENTION_WRITE")
    assert a.token == b.token
    lines = (tmp_path / "tokens.jsonl").read_text(encoding="utf-8").splitlines()
    assert len(lines) == 1


def test_section_f_capability_set_is_complete() -> None:
    """Every CAP_ token named in Section F must be registerable."""
    for cap in (
        "CAP_NETWORK_SYNC", "CAP_EXPERIMENT_WRITE", "CAP_TOOL_COMPOSE",
        "CAP_INVENTION_WRITE", "CAP_CAPABILITY_PROBE", "CAP_COUNTERFACTUAL",
        "CAP_CONSTRAINT_WRITE", "CAP_SAFETY_REVIEW", "CAP_CONTRACT_WRITE",
        "CAP_ANALOGY_READ", "CAP_SELF_IMPROVEMENT_PROPOSE", "CAP_PLAN_WRITE",
        "CAP_MODEL_ROUTE", "CAP_WORLD_MODEL_WRITE", "CAP_META_LEARN",
        "CAP_CAUSAL_WRITE", "CAP_HYPOTHESIS_WRITE", "CAP_ALIGNMENT_MONITOR",
        "CAP_PARALLEL_EXEC", "CAP_WATCHDOG", "CAP_MEMORY_WRITE",
        "CAP_SUBSTRATE", "CAP_LOAD_BALANCE", "CAP_ETHICS_DELIBERATE",
        "CAP_ONTOLOGY_WRITE",
    ):
        assert cap in KNOWN_CAPABILITIES
