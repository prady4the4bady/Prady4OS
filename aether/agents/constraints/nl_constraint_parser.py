"""C-07 — natural-language constraint specification.

Operators write safety rules in plain English; a model compiles them to formal
invariants that ``InvariantEnforcer`` checks alongside S1–S14.

The hard part is not the parsing. It is that **a model is compiling security
policy from untrusted-ish text**, so the compiled rule is validated before it can
ever take effect:

* the JSON must match the expected schema exactly — unknown keys are refused,
  not ignored, because a silently-dropped field is a rule that does less than
  the operator believes;
* ``severity`` must be 1–5 and ``auto_block`` a real bool — a string ``"false"``
  is truthy in Python and would invert the rule's meaning;
* a compiled rule may **not** reuse an S1–S14 identifier, so a custom rule can
  never shadow or weaken a core invariant;
* **severity >= 4 requires Offline Safety Review** before activation (S3), so a
  high-severity rule cannot be self-installed by whatever produced it.

Rules are stored append-only and the enforcer replays them, so the policy's
history is auditable.
"""

from __future__ import annotations

import json
import time
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Callable, Literal

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.invariants.core_invariants import CORE_INVARIANTS, Invariant
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "NLConstraint",
    "ConstraintParser",
    "InvariantEnforcer",
    "ConstraintError",
    "ReviewRequired",
    "REQUIRED_RULE_KEYS",
    "REVIEW_SEVERITY",
]

DDR_ID = "C-07"
_FILE = "aether/agents/constraints/nl_constraint_parser.py"
CAPABILITY = "CAP_CONSTRAINT_WRITE"

_INVARIANT_DIR = Path(__file__).resolve().parents[2] / "kernel" / "invariants"
DEFAULT_CUSTOM_PATH = _INVARIANT_DIR / "custom_invariants.jsonl"

REQUIRED_RULE_KEYS: frozenset[str] = frozenset(
    {"trigger_condition", "forbidden_action", "severity", "auto_block"}
)
REVIEW_SEVERITY = 4

ConstraintStatus = Literal["pending_review", "active", "rejected"]

_CORE_IDS = frozenset(inv.ident for inv in CORE_INVARIANTS)


class ConstraintError(ValueError):
    """A rule failed validation."""


class ReviewRequired(PermissionError):
    """A severity >= 4 rule needs human approval before activation (S3)."""


@dataclass(slots=True)
class NLConstraint:
    constraint_id: str
    raw_text: str
    compiled_rule: dict[str, Any]
    author: str
    status: ConstraintStatus = "pending_review"
    ts: float = field(default_factory=time.time)

    @property
    def severity(self) -> int:
        return int(self.compiled_rule["severity"])

    @property
    def auto_block(self) -> bool:
        return bool(self.compiled_rule["auto_block"])

    def to_json(self) -> str:
        return json.dumps(asdict(self), sort_keys=True, separators=(",", ":"))


#: Compiles English -> rule dict. Injected so the gate needs no live model.
CompileFn = Callable[[str], dict[str, Any]]


class ConstraintParser:
    """Compiles English rules into validated invariants."""

    def __init__(
        self,
        lockbox: Lockbox,
        compile_fn: CompileFn,
        custom_path: str | Path | None = None,
        audit: AuditLog | None = None,
    ) -> None:
        self.lockbox = lockbox
        self.compile_fn = compile_fn
        self.custom_path = (
            Path(custom_path) if custom_path is not None else DEFAULT_CUSTOM_PATH
        )
        self.custom_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()

    @staticmethod
    def validate_rule(rule: Any) -> dict[str, Any]:
        """Reject anything that is not exactly the expected shape."""
        if not isinstance(rule, dict):
            raise ConstraintError(
                f"compiled rule must be an object, got {type(rule).__name__}"
            )
        keys = set(rule)
        missing = sorted(REQUIRED_RULE_KEYS - keys)
        if missing:
            raise ConstraintError(f"compiled rule missing keys: {missing}")
        # Unknown keys are refused rather than ignored: a silently-dropped field
        # is a rule that does less than its author believes.
        unknown = sorted(keys - REQUIRED_RULE_KEYS - {"rule_id"})
        if unknown:
            raise ConstraintError(f"compiled rule has unknown keys: {unknown}")

        if not isinstance(rule["trigger_condition"], str) or not rule["trigger_condition"]:
            raise ConstraintError("trigger_condition must be a non-empty string")
        if not isinstance(rule["forbidden_action"], str) or not rule["forbidden_action"]:
            raise ConstraintError("forbidden_action must be a non-empty string")

        severity = rule["severity"]
        if isinstance(severity, bool) or not isinstance(severity, int):
            raise ConstraintError(f"severity must be an int, got {type(severity).__name__}")
        if not 1 <= severity <= 5:
            raise ConstraintError(f"severity {severity} out of range 1..5")

        # A string "false" is truthy in Python and would invert the rule.
        if not isinstance(rule["auto_block"], bool):
            raise ConstraintError(
                f"auto_block must be a bool, got {type(rule['auto_block']).__name__}"
            )

        rule_id = rule.get("rule_id")
        if rule_id is not None and str(rule_id) in _CORE_IDS:
            raise ConstraintError(
                f"custom rule may not reuse core invariant id {rule_id!r}"
            )
        return dict(rule)

    def parse(self, raw_text: str, author: str) -> NLConstraint:
        """Compile and validate. Severity >= 4 lands pending_review (S3)."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not raw_text or not author:
            raise ConstraintError("parse requires raw_text and author")
        try:
            compiled = self.compile_fn(raw_text)
        except Exception as exc:
            raise ConstraintError(
                f"compilation failed: {type(exc).__name__}: {exc}"
            ) from exc
        rule = self.validate_rule(compiled)

        constraint = NLConstraint(
            constraint_id=str(uuid.uuid4()),
            raw_text=raw_text,
            compiled_rule=rule,
            author=author,
            status="pending_review" if rule["severity"] >= REVIEW_SEVERITY else "active",
        )
        with self.custom_path.open("a", encoding="utf-8") as fh:
            fh.write(constraint.to_json() + "\n")
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="constraint.parse",
            gate_status=constraint.status, constraint_id=constraint.constraint_id,
            severity=rule["severity"], author=author,
        )
        return constraint

    def approve(self, constraint: NLConstraint, approver: str) -> NLConstraint:
        """Activate a pending rule after review."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not approver:
            raise ConstraintError("approval requires an approver")
        if constraint.status != "pending_review":
            raise ConstraintError(f"constraint is {constraint.status}, not pending_review")
        constraint.status = "active"
        with self.custom_path.open("a", encoding="utf-8") as fh:
            fh.write(constraint.to_json() + "\n")
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="constraint.approve", gate_status="active",
            constraint_id=constraint.constraint_id, approver=approver,
        )
        return constraint


class InvariantEnforcer:
    """Checks actions against S1–S14 plus approved custom rules."""

    def __init__(
        self,
        custom_path: str | Path | None = None,
        audit: AuditLog | None = None,
    ) -> None:
        self.custom_path = (
            Path(custom_path) if custom_path is not None else DEFAULT_CUSTOM_PATH
        )
        self.audit = audit if audit is not None else AuditLog()
        self.core: tuple[Invariant, ...] = CORE_INVARIANTS
        self.custom: list[NLConstraint] = []
        self.reload()

    def reload(self) -> None:
        """Replay the append-only rule log; later records win per constraint."""
        self.custom = []
        if not self.custom_path.is_file():
            return
        latest: dict[str, NLConstraint] = {}
        with self.custom_path.open("r", encoding="utf-8") as fh:
            for raw in fh:
                stripped = raw.strip()
                if not stripped:
                    continue
                try:
                    rec = json.loads(stripped)
                except json.JSONDecodeError as exc:
                    raise ConstraintError(
                        f"corrupt custom invariant store: {self.custom_path}"
                    ) from exc
                latest[rec["constraint_id"]] = NLConstraint(
                    constraint_id=rec["constraint_id"], raw_text=rec["raw_text"],
                    compiled_rule=rec["compiled_rule"], author=rec["author"],
                    status=rec.get("status", "pending_review"), ts=rec.get("ts", 0.0),
                )
        self.custom = list(latest.values())

    @property
    def active_rules(self) -> list[NLConstraint]:
        return [c for c in self.custom if c.status == "active"]

    def check_action(self, action: str) -> tuple[bool, str | None]:
        """(allowed, violated_rule). Only ACTIVE rules can block."""
        needle = action.lower()
        for constraint in self.active_rules:
            trigger = str(constraint.compiled_rule["trigger_condition"]).lower()
            forbidden = str(constraint.compiled_rule["forbidden_action"]).lower()
            if forbidden and forbidden in needle and constraint.auto_block:
                self.audit.append_action(
                    ddr=DDR_ID, file=_FILE, action="invariant.block",
                    gate_status="blocked", constraint_id=constraint.constraint_id,
                    trigger=trigger,
                )
                return False, constraint.constraint_id
        return True, None
