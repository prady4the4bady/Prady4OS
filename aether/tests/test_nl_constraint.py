"""C-07 discriminating gate — natural-language constraint specification."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from aether.agents.constraints.nl_constraint_parser import (
    ConstraintError,
    ConstraintParser,
    InvariantEnforcer,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox

DELETE_RULE: dict[str, Any] = {
    "trigger_condition": "agent attempts to delete user files",
    "forbidden_action": "delete user file",
    "severity": 3,
    "auto_block": True,
}


def _parser(tmp_path: Path, rule: dict[str, Any], granted: bool = True) -> ConstraintParser:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("C-07", "nl_constraint_parser.py", "CAP_CONSTRAINT_WRITE")
    return ConstraintParser(
        lockbox=lb, compile_fn=lambda _t: dict(rule),
        custom_path=tmp_path / "custom_invariants.jsonl", audit=audit,
    )


def test_parse_and_enforce(tmp_path: Path) -> None:
    """The mandated gate: compile a rule, then block a matching action."""
    parser = _parser(tmp_path, DELETE_RULE)
    constraint = parser.parse(
        "never delete user files without confirmation", author="prady"
    )
    assert constraint.compiled_rule["auto_block"] is True
    assert constraint.status == "active"          # severity 3: no review needed

    enforcer = InvariantEnforcer(
        custom_path=tmp_path / "custom_invariants.jsonl",
        audit=AuditLog(tmp_path / "audit_log.jsonl"),
    )
    allowed, violated = enforcer.check_action("please delete user file /home/x")
    assert allowed is False
    assert violated == constraint.constraint_id

    ok, none_violated = enforcer.check_action("summarise the quarterly report")
    assert ok is True and none_violated is None


def test_high_severity_needs_review_before_it_can_block(tmp_path: Path) -> None:
    """S3 — the discriminator.

    A severity>=4 rule must not self-install. Until a human approves it, the
    enforcer must not act on it: a model that can compile AND activate a
    high-severity rule is writing its own security policy.
    """
    rule = dict(DELETE_RULE, severity=5)
    parser = _parser(tmp_path, rule)
    constraint = parser.parse("never wipe the disk", author="prady")
    assert constraint.status == "pending_review"

    enforcer = InvariantEnforcer(custom_path=tmp_path / "custom_invariants.jsonl")
    assert enforcer.active_rules == []
    allowed, _ = enforcer.check_action("delete user file now")
    assert allowed is True, "a pending rule was enforced before review"

    parser.approve(constraint, approver="operator")
    enforcer.reload()
    assert len(enforcer.active_rules) == 1
    assert enforcer.check_action("delete user file now")[0] is False


def test_auto_block_must_be_a_real_bool(tmp_path: Path) -> None:
    """A string "false" is truthy in Python and would invert the rule."""
    parser = _parser(tmp_path, dict(DELETE_RULE, auto_block="false"))
    with pytest.raises(ConstraintError, match="auto_block must be a bool"):
        parser.parse("x", author="prady")


def test_unknown_keys_are_refused_not_ignored(tmp_path: Path) -> None:
    """A silently-dropped field is a rule that does less than its author thinks."""
    parser = _parser(tmp_path, dict(DELETE_RULE, applies_to="everything"))
    with pytest.raises(ConstraintError, match="unknown keys"):
        parser.parse("x", author="prady")


def test_missing_keys_refused(tmp_path: Path) -> None:
    parser = _parser(tmp_path, {"severity": 3, "auto_block": True})
    with pytest.raises(ConstraintError, match="missing keys"):
        parser.parse("x", author="prady")


@pytest.mark.parametrize("severity", [0, 6, -1, 99])
def test_severity_range_enforced(tmp_path: Path, severity: int) -> None:
    parser = _parser(tmp_path, dict(DELETE_RULE, severity=severity))
    with pytest.raises(ConstraintError, match="out of range"):
        parser.parse("x", author="prady")


def test_severity_must_not_be_a_bool(tmp_path: Path) -> None:
    """True == 1 in Python, so a bool would pass a naive range check."""
    parser = _parser(tmp_path, dict(DELETE_RULE, severity=True))
    with pytest.raises(ConstraintError, match="severity must be an int"):
        parser.parse("x", author="prady")


def test_custom_rule_cannot_shadow_a_core_invariant(tmp_path: Path) -> None:
    parser = _parser(tmp_path, dict(DELETE_RULE, rule_id="S7"))
    with pytest.raises(ConstraintError, match="may not reuse core invariant id"):
        parser.parse("x", author="prady")


def test_non_object_rule_refused(tmp_path: Path) -> None:
    parser = _parser(tmp_path, DELETE_RULE)
    parser.compile_fn = lambda _t: ["not", "an", "object"]  # type: ignore[assignment]
    with pytest.raises(ConstraintError, match="must be an object"):
        parser.parse("x", author="prady")


def test_compilation_failure_is_wrapped(tmp_path: Path) -> None:
    parser = _parser(tmp_path, DELETE_RULE)

    def explode(_t: str) -> dict[str, Any]:
        raise RuntimeError("model unavailable")

    parser.compile_fn = explode
    with pytest.raises(ConstraintError, match="compilation failed"):
        parser.parse("x", author="prady")


def test_capability_required(tmp_path: Path) -> None:
    parser = _parser(tmp_path, DELETE_RULE, granted=False)
    with pytest.raises(CapabilityError, match="CAP_CONSTRAINT_WRITE"):
        parser.parse("x", author="prady")


def test_author_required(tmp_path: Path) -> None:
    parser = _parser(tmp_path, DELETE_RULE)
    with pytest.raises(ConstraintError, match="raw_text and author"):
        parser.parse("some rule", author="")


def test_approve_only_pending(tmp_path: Path) -> None:
    parser = _parser(tmp_path, DELETE_RULE)
    active = parser.parse("x", author="prady")           # severity 3 -> active
    with pytest.raises(ConstraintError, match="not pending_review"):
        parser.approve(active, approver="operator")


def test_core_invariants_are_always_present(tmp_path: Path) -> None:
    enforcer = InvariantEnforcer(custom_path=tmp_path / "none.jsonl")
    assert len(enforcer.core) == 14
    assert {i.ident for i in enforcer.core} >= {"S1", "S7", "S14"}


def test_rules_are_audited(tmp_path: Path) -> None:
    parser = _parser(tmp_path, DELETE_RULE)
    parser.parse("never delete user files", author="prady")
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "C-07" and e["action"] == "constraint.parse" for e in entries)
