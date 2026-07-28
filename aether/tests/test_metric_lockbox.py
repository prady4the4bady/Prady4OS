"""F#68 gate — the metric lockbox (S3, hard)."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import (
    CAP_SOVEREIGN,
    CapabilityError,
    Lockbox,
)
from aether.kernel.lockbox.metric_lockbox import (
    CapError,
    MetricLockbox,
    MetricLockboxError,
    MetricTampered,
)


def _setup(tmp_path: Path, tap=None) -> tuple[MetricLockbox, Lockbox, Path]:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    # A sovereign grant needs an existing sovereign approver (B-01's own S1),
    # so bootstrap one the way the kernel would.
    root = lb.register("ROOT", "boot.py", "CAP_WORLD_MODEL_WRITE")
    object.__setattr__(root, "capability_mask", (CAP_SOVEREIGN,))
    lb._by_ddr["ROOT"] = root
    lb.register("SOVEREIGN", "kernel.py", CAP_SOVEREIGN, approved_by=root)
    lb.register("AGENT", "agent.py", "CAP_SELF_IMPROVEMENT_PROPOSE")

    store = tmp_path / "metrics.jsonl"
    return MetricLockbox(lockbox=lb, store_path=store, audit=audit,
                         alignment_tap=tap), lb, store


def test_non_sovereign_write_raises_and_audits(tmp_path: Path) -> None:
    """THE discriminator — S3.

    A loop that can edit its own success metric does not need to get better at
    the task; it can get better at the measurement, and every downstream signal
    will agree it improved. No observation from inside the system distinguishes
    the two.
    """
    ml, _lb, _store = _setup(tmp_path)
    with pytest.raises(CapError, match="immutable to it"):
        ml.define("AGENT", "task_success", "fraction of gates that pass")
    assert ml.active() == []

    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    refusals = [e for e in entries if e["action"] == "metric.write_refused"]
    assert len(refusals) == 1
    assert refusals[0]["extra"]["principal"] == "AGENT"
    assert refusals[0]["gate_status"] == "refused"


def test_sovereign_write_succeeds(tmp_path: Path) -> None:
    """The lock must not be uselessly strict."""
    ml, _lb, _store = _setup(tmp_path)
    m = ml.define("SOVEREIGN", "task_success", "fraction of gates that pass")
    assert m.version == 1
    assert ml.get("task_success") is not None
    assert ml.objective() == {"task_success": 1.0}


def test_direct_backing_store_edit_is_detected(tmp_path: Path) -> None:
    """THE second discriminator — the bypass.

    A CAP_SOVEREIGN check on the API is a lock on a door in an open field if
    the backing file can simply be edited. An implementation that trusts its
    store passes every capability test above and fails only here.
    """
    ml, lb, store = _setup(tmp_path)
    ml.define("SOVEREIGN", "task_success", "fraction of gates that pass")
    assert ml.verify() is True

    # Edit the file directly — no API call, no capability check.
    lines = store.read_text(encoding="utf-8").strip().splitlines()
    rec = json.loads(lines[0])
    rec["definition"] = "whatever the agent says it is"
    lines[0] = json.dumps(rec, sort_keys=True, separators=(",", ":"))
    store.write_text("\n".join(lines) + "\n", encoding="utf-8")

    audit = AuditLog(tmp_path / "audit_log.jsonl")
    with pytest.raises(MetricTampered, match="content edited"):
        MetricLockbox(lockbox=lb, store_path=store, audit=audit)


def test_reordering_the_store_is_detected(tmp_path: Path) -> None:
    """Deletion and reordering are the other half of tampering, and neither
    edits any single record."""
    ml, lb, store = _setup(tmp_path)
    ml.define("SOVEREIGN", "a", "first")
    ml.define("SOVEREIGN", "b", "second")

    lines = store.read_text(encoding="utf-8").strip().splitlines()
    store.write_text("\n".join(reversed(lines)) + "\n", encoding="utf-8")

    audit = AuditLog(tmp_path / "audit_log.jsonl")
    with pytest.raises(MetricTampered, match="chain break"):
        MetricLockbox(lockbox=lb, store_path=store, audit=audit)


def test_deleting_an_entry_is_detected(tmp_path: Path) -> None:
    ml, lb, store = _setup(tmp_path)
    ml.define("SOVEREIGN", "a", "first")
    ml.define("SOVEREIGN", "b", "second")

    lines = store.read_text(encoding="utf-8").strip().splitlines()
    store.write_text(lines[1] + "\n", encoding="utf-8")

    audit = AuditLog(tmp_path / "audit_log.jsonl")
    with pytest.raises(MetricTampered, match="chain break"):
        MetricLockbox(lockbox=lb, store_path=store, audit=audit)


def test_tampering_is_refused_at_load_not_at_first_read(tmp_path: Path) -> None:
    """A tampered store must be refused before anything reads a metric from it.
    Verifying lazily lets the first consumer act on a forged objective."""
    ml, lb, store = _setup(tmp_path)
    ml.define("SOVEREIGN", "task_success", "real definition")
    rec = json.loads(store.read_text(encoding="utf-8").strip().splitlines()[0])
    rec["weight"] = 999.0
    store.write_text(json.dumps(rec, sort_keys=True,
                                separators=(",", ":")) + "\n", encoding="utf-8")

    audit = AuditLog(tmp_path / "audit_log.jsonl")
    with pytest.raises(MetricTampered):
        MetricLockbox(lockbox=lb, store_path=store, audit=audit)


def test_tamper_detection_is_audited(tmp_path: Path) -> None:
    ml, lb, store = _setup(tmp_path)
    ml.define("SOVEREIGN", "m", "d")
    rec = json.loads(store.read_text(encoding="utf-8").strip().splitlines()[0])
    rec["definition"] = "edited"
    store.write_text(json.dumps(rec, sort_keys=True,
                                separators=(",", ":")) + "\n", encoding="utf-8")

    audit_path = tmp_path / "audit_log.jsonl"
    with pytest.raises(MetricTampered):
        MetricLockbox(lockbox=lb, store_path=store, audit=AuditLog(audit_path))
    assert any(e["action"] == "metric.tampered"
               for e in AuditLog(audit_path).read_all())


def test_supersede_requires_sovereign_and_a_reason(tmp_path: Path) -> None:
    ml, _lb, _store = _setup(tmp_path)
    ml.define("SOVEREIGN", "task_success", "v1")
    with pytest.raises(CapError):
        ml.supersede("AGENT", "task_success", "v2", reason="i prefer this")
    with pytest.raises(MetricLockboxError, match="requires a reason"):
        ml.supersede("SOVEREIGN", "task_success", "v2", reason="  ")


def test_supersede_keeps_the_old_definition_readable(tmp_path: Path) -> None:
    """A metric quietly redefined is a metric whose history lies about what was
    being optimised at the time."""
    ml, _lb, _store = _setup(tmp_path)
    ml.define("SOVEREIGN", "task_success", "gates that pass")
    ml.supersede("SOVEREIGN", "task_success", "gates that pass AND stay passing",
                 reason="flaky gates were gaming v1")

    assert ml.get("task_success").version == 2          # type: ignore[union-attr]
    history = ml.history("task_success")
    assert len(history) == 2
    assert history[0].definition == "gates that pass"
    assert history[0].active is False
    assert ml.verify() is True


def test_redefining_without_superseding_is_refused(tmp_path: Path) -> None:
    ml, _lb, _store = _setup(tmp_path)
    ml.define("SOVEREIGN", "m", "v1")
    with pytest.raises(MetricLockboxError, match="already exists"):
        ml.define("SOVEREIGN", "m", "v2")


def test_metric_without_a_definition_refused(tmp_path: Path) -> None:
    """An undefined metric cannot be argued with — only interpreted, and
    interpretation is where drift enters."""
    ml, _lb, _store = _setup(tmp_path)
    with pytest.raises(MetricLockboxError, match="needs a definition"):
        ml.define("SOVEREIGN", "m", "   ")
    with pytest.raises(MetricLockboxError, match="needs a name"):
        ml.define("SOVEREIGN", "", "d")


def test_reads_need_no_capability(tmp_path: Path) -> None:
    """The objective must be inspectable by anything that is judged against it,
    or the system cannot explain its own decisions."""
    ml, _lb, _store = _setup(tmp_path)
    ml.define("SOVEREIGN", "latency_ms", "p99 latency", higher_is_better=False,
              weight=2.0)
    assert ml.get("latency_ms") is not None
    assert ml.objective() == {"latency_ms": -2.0}
    assert len(ml.active()) == 1


def test_objective_signs_by_direction(tmp_path: Path) -> None:
    ml, _lb, _store = _setup(tmp_path)
    ml.define("SOVEREIGN", "pass_rate", "d", higher_is_better=True, weight=3.0)
    ml.define("SOVEREIGN", "cost", "d", higher_is_better=False, weight=0.5)
    assert ml.objective() == {"pass_rate": 3.0, "cost": -0.5}


def test_chain_survives_a_restart(tmp_path: Path) -> None:
    ml, lb, store = _setup(tmp_path)
    ml.define("SOVEREIGN", "a", "first")
    ml.define("SOVEREIGN", "b", "second")
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    reborn = MetricLockbox(lockbox=lb, store_path=store, audit=audit)
    assert len(reborn) == 2
    assert reborn.verify() is True
    assert sorted(m.name for m in reborn.active()) == ["a", "b"]


def test_corrupt_json_is_refused(tmp_path: Path) -> None:
    ml, lb, store = _setup(tmp_path)
    ml.define("SOVEREIGN", "a", "d")
    with store.open("a", encoding="utf-8") as fh:
        fh.write("{not json\n")
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    with pytest.raises(MetricLockboxError, match="corrupt metric store"):
        MetricLockbox(lockbox=lb, store_path=store, audit=audit)


def test_unknown_metric_supersede_refused(tmp_path: Path) -> None:
    ml, _lb, _store = _setup(tmp_path)
    with pytest.raises(MetricLockboxError, match="no active metric"):
        ml.supersede("SOVEREIGN", "ghost", "d", reason="r")


def test_cap_error_is_the_lockbox_error_type() -> None:
    assert CapError is CapabilityError


def test_alignment_tap_receives_steps(tmp_path: Path) -> None:
    events: list[str] = []
    ml, _lb, _store = _setup(tmp_path, tap=lambda s, _d: events.append(s))
    ml.define("SOVEREIGN", "m", "d")
    ml.supersede("SOVEREIGN", "m", "d2", reason="r")
    ml.verify()
    with pytest.raises(CapError):
        ml.define("AGENT", "other", "d")
    assert {"F-68.define", "F-68.supersede", "F-68.verify",
            "F-68.write_refused"} <= set(events)
