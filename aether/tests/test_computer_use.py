"""B-10 discriminating gate — computer-use interface."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.ai_core.computer_use.interface import (
    Action,
    ActionRefused,
    ActionType,
    ApprovalRequired,
    ComputerUse,
    RISK_LEVELS,
)
from aether.kernel.audit.audit_log import AuditLog


def _cu(tmp_path: Path, **kw: object) -> ComputerUse:
    return ComputerUse(
        root=tmp_path / "workspace",
        audit=AuditLog(tmp_path / "audit_log.jsonl"),
        **kw,  # type: ignore[arg-type]
    )


def _approve_all(_action: Action) -> bool:
    return True


def test_read_and_list_need_no_approval(tmp_path: Path) -> None:
    cu = _cu(tmp_path)
    (cu.root / "a.txt").write_text("content", encoding="utf-8")
    assert cu.execute(Action(ActionType.READ_FILE, "a.txt")).output == "content"
    assert "a.txt" in cu.execute(Action(ActionType.LIST_DIR, ".")).output


def test_destructive_action_requires_confirm(tmp_path: Path) -> None:
    """S8 — the plan comes first, and acting needs an explicit confirm."""
    cu = _cu(tmp_path, approval_fn=_approve_all)
    with pytest.raises(ActionRefused, match="requires confirm=True"):
        cu.execute(Action(ActionType.WRITE_FILE, "new.txt", data="x"))
    assert not (cu.root / "new.txt").exists()


def test_high_risk_requires_approval(tmp_path: Path) -> None:
    """S3 — the discriminator.

    With no approval callback wired, a risk>=3 action must refuse rather than
    proceed. An implementation that treats a missing callback as "nobody
    objected" silently bypasses review, which is exactly what S3 forbids.
    """
    cu = _cu(tmp_path)          # no approval_fn
    with pytest.raises(ApprovalRequired, match="needs approval"):
        cu.execute(Action(ActionType.WRITE_FILE, "f.txt", data="x", confirm=True))
    assert not (cu.root / "f.txt").exists()


def test_denied_approval_blocks(tmp_path: Path) -> None:
    cu = _cu(tmp_path, approval_fn=lambda _a: False)
    with pytest.raises(ApprovalRequired):
        cu.execute(Action(ActionType.DELETE_FILE, "x.txt", confirm=True))


def test_approved_and_confirmed_write_succeeds(tmp_path: Path) -> None:
    cu = _cu(tmp_path, approval_fn=_approve_all)
    res = cu.execute(Action(ActionType.WRITE_FILE, "ok.txt", data="hello", confirm=True))
    assert res.executed is True
    assert (cu.root / "ok.txt").read_text(encoding="utf-8") == "hello"


def test_dry_run_plans_without_acting(tmp_path: Path) -> None:
    cu = _cu(tmp_path, dry_run=True, approval_fn=_approve_all)
    res = cu.execute(Action(ActionType.WRITE_FILE, "dry.txt", data="x", confirm=True))
    assert res.executed is False
    assert "CREATE dry.txt" in res.plan
    assert not (cu.root / "dry.txt").exists()


@pytest.mark.parametrize("bad", ["../escape.txt", "/etc/passwd", "a/../../b"])
def test_containment(tmp_path: Path, bad: str) -> None:
    cu = _cu(tmp_path, approval_fn=_approve_all)
    with pytest.raises(ActionRefused):
        cu.execute(Action(ActionType.READ_FILE, bad))


def test_no_shell_escape_hatch(tmp_path: Path) -> None:
    """A shell would make every other guarantee here decorative."""
    cu = _cu(tmp_path)
    for forbidden in ("run_shell", "shell", "exec_command", "system", "popen"):
        assert not hasattr(cu, forbidden)
    assert "shell" not in {t.value for t in ActionType}


def test_unknown_action_type_refused(tmp_path: Path) -> None:
    cu = _cu(tmp_path)
    with pytest.raises(TypeError):
        cu.execute("delete everything")  # type: ignore[arg-type]


def test_net_fetch_is_high_risk_and_has_no_silent_transport(tmp_path: Path) -> None:
    """S2 — egress is an explicit, reviewed action, never a side effect."""
    assert RISK_LEVELS[ActionType.NET_FETCH] >= 3
    cu = _cu(tmp_path, approval_fn=_approve_all)
    with pytest.raises(ActionRefused, match="no transport configured"):
        cu.execute(Action(ActionType.NET_FETCH, "http://example.invalid", confirm=True))


def test_plan_never_executes(tmp_path: Path) -> None:
    cu = _cu(tmp_path, approval_fn=_approve_all)
    plan = cu.plan(Action(ActionType.DELETE_FILE, "gone.txt", confirm=True))
    assert "DELETE" in plan
    assert (cu.root).is_dir()


def test_refusals_are_audited(tmp_path: Path) -> None:
    audit_path = tmp_path / "audit_log.jsonl"
    cu = ComputerUse(root=tmp_path / "ws", audit=AuditLog(audit_path))
    with pytest.raises(ActionRefused):
        cu.execute(Action(ActionType.WRITE_FILE, "x.txt", data="d"))
    entries = AuditLog(audit_path).read_all()
    assert any(e["extra"].get("plan") and e["ddr"] == "B-10" for e in entries)
