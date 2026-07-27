"""C-04 discriminating gate — invention registry."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.agents.invention.registry import (
    AUTO_RETIRE_AFTER_S,
    InventionError,
    InventionRegistry,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _registry(
    tmp_path: Path, granted: bool = True, now: float | None = None
) -> InventionRegistry:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("C-04", "registry.py", "CAP_INVENTION_WRITE")
    return InventionRegistry(
        lockbox=lb, store_path=tmp_path / "registry.jsonl", audit=audit,
        now_fn=(lambda: now) if now is not None else None,
    )


def test_add_deprecate_retire(tmp_path: Path) -> None:
    """The mandated gate: add one invention, deprecate it, verify status."""
    reg = _registry(tmp_path)
    inv_id = reg.add(
        source_ddr="C-03",
        title="fetch+summarise pipeline",
        description="composed at runtime",
        capability_delta={"tools": ["fetch_document", "summarise_text"]},
    )
    inv = reg.get(inv_id)
    assert inv is not None
    assert inv.status == "active"
    assert len(reg.list_active()) == 1

    reg.deprecate(inv_id, reason="superseded by a better chain")
    assert reg.get(inv_id).status == "deprecated"   # type: ignore[union-attr]
    assert reg.list_active() == []

    reg.retire(inv_id, reason="no longer referenced")
    assert reg.get(inv_id).status == "retired"      # type: ignore[union-attr]


def test_status_change_appends_never_edits(tmp_path: Path) -> None:
    """The discriminator.

    A registry of self-generated capabilities must keep its own history
    auditable. An implementation that rewrites the record in place leaves one
    line and destroys the evidence of what changed and why.
    """
    reg = _registry(tmp_path)
    inv_id = reg.add("C-03", "a pipeline")
    reg.deprecate(inv_id, reason="superseded")

    lines = (tmp_path / "registry.jsonl").read_text(encoding="utf-8").splitlines()
    assert len(lines) == 2, "status change did not append a new record"
    assert '"status":"active"' in lines[0].replace(" ", "")
    assert '"status":"deprecated"' in lines[1].replace(" ", "")


def test_state_is_derived_by_replay(tmp_path: Path) -> None:
    reg = _registry(tmp_path)
    inv_id = reg.add("C-03", "a pipeline")
    reg.deprecate(inv_id, reason="superseded")

    reborn = _registry(tmp_path)
    inv = reborn.get(inv_id)
    assert inv is not None
    assert inv.status == "deprecated"          # later record won
    assert inv.reason == "superseded"


def test_transitions_require_a_reason(tmp_path: Path) -> None:
    reg = _registry(tmp_path)
    inv_id = reg.add("C-03", "a pipeline")
    with pytest.raises(InventionError, match="requires a reason"):
        reg.deprecate(inv_id, reason="")


def test_retired_is_terminal(tmp_path: Path) -> None:
    reg = _registry(tmp_path)
    inv_id = reg.add("C-03", "a pipeline")
    reg.retire(inv_id, reason="done")
    with pytest.raises(InventionError, match="cannot change status"):
        reg.deprecate(inv_id, reason="undo")


def test_auto_retire_uses_an_injected_clock(tmp_path: Path) -> None:
    """A 30-day rule that needs a 30-day wait to test is never tested."""
    clock = {"t": 1_000_000.0}
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    lb.register("C-04", "registry.py", "CAP_INVENTION_WRITE")
    reg = InventionRegistry(
        lockbox=lb, store_path=tmp_path / "registry.jsonl", audit=audit,
        now_fn=lambda: clock["t"],
    )
    fresh = reg.add("C-03", "recent")
    stale = reg.add("C-03", "forgotten")

    clock["t"] += AUTO_RETIRE_AFTER_S + 86400.0
    reg.mark_used(fresh)                         # touched at the new "now"
    retired = reg.auto_retire()

    assert [i.inv_id for i in retired] == [stale]
    assert reg.get(fresh).status == "active"     # type: ignore[union-attr]
    assert "auto" in reg.get(stale).reason       # type: ignore[union-attr]


def test_auto_retire_is_a_noop_when_everything_is_fresh(tmp_path: Path) -> None:
    reg = _registry(tmp_path)
    reg.add("C-03", "recent")
    assert reg.auto_retire() == []


def test_capability_required(tmp_path: Path) -> None:
    reg = _registry(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_INVENTION_WRITE"):
        reg.add("C-03", "a pipeline")


def test_title_required(tmp_path: Path) -> None:
    reg = _registry(tmp_path)
    with pytest.raises(InventionError, match="needs a title"):
        reg.add("C-03", "")


def test_unknown_invention_raises(tmp_path: Path) -> None:
    reg = _registry(tmp_path)
    with pytest.raises(InventionError, match="unknown invention"):
        reg.deprecate("no-such-id", reason="x")
    with pytest.raises(InventionError, match="unknown invention"):
        reg.mark_used("no-such-id")


def test_ids_are_unique(tmp_path: Path) -> None:
    reg = _registry(tmp_path)
    ids = {reg.add("C-03", f"pipeline {i}") for i in range(20)}
    assert len(ids) == 20


def test_additions_are_audited(tmp_path: Path) -> None:
    reg = _registry(tmp_path)
    reg.add("C-03", "a pipeline")
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "C-04" and e["action"] == "invention.add" for e in entries)


def test_corrupt_store_reported(tmp_path: Path) -> None:
    (tmp_path / "registry.jsonl").write_text("{bad json\n", encoding="utf-8")
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    with pytest.raises(InventionError, match="corrupt invention registry"):
        InventionRegistry(lockbox=lb, store_path=tmp_path / "registry.jsonl", audit=audit)
