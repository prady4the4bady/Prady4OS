"""B-10 — computer-use interface.

The layer's most dangerous surface: it lets an agent act on the machine. So the
design is refusal-first — the interface knows a small set of action types, and
anything it does not recognise is refused rather than passed through to a shell.

Invariants this carries:

* **S8 — no irreversible filesystem change without dry-run.** Destructive
  actions require ``confirm=True`` *and* produce a dry-run plan first. In
  ``dry_run`` mode nothing executes.
* **S3 — no bypassing review for risk >= 3.** Each action type has a fixed risk
  level; anything at or above the threshold routes to an approval callback
  before it can run. C-08 supplies the real queue later; the hook exists now so
  the path is never optional.
* **S2 — no exfiltration without consent.** Network egress is an explicit action
  type with its own risk, not a side effect of "run a command".

There is deliberately no ``run_shell`` escape hatch. A shell would make every
other guarantee here decorative.
"""

from __future__ import annotations

import shutil
import time
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, Callable

from aether.kernel.audit.audit_log import AuditLog

__all__ = [
    "ActionType",
    "Action",
    "ActionResult",
    "ComputerUse",
    "ActionRefused",
    "ApprovalRequired",
    "RISK_LEVELS",
]

DDR_ID = "B-10"
_FILE = "aether/ai_core/computer_use/interface.py"

REVIEW_RISK_THRESHOLD = 3


class ActionRefused(PermissionError):
    """The action is not permitted (unknown type, containment, missing confirm)."""


class ApprovalRequired(PermissionError):
    """The action needs human approval and none was granted (S3)."""


class ActionType(str, Enum):
    READ_FILE = "read_file"
    LIST_DIR = "list_dir"
    WRITE_FILE = "write_file"
    DELETE_FILE = "delete_file"
    NET_FETCH = "net_fetch"


#: Fixed per type. Risk is a property of the action, not of who is asking.
RISK_LEVELS: dict[ActionType, int] = {
    ActionType.READ_FILE: 1,
    ActionType.LIST_DIR: 1,
    ActionType.WRITE_FILE: 3,
    ActionType.DELETE_FILE: 5,
    ActionType.NET_FETCH: 4,
}

#: Types that change or leak state irreversibly.
DESTRUCTIVE: frozenset[ActionType] = frozenset(
    {ActionType.WRITE_FILE, ActionType.DELETE_FILE, ActionType.NET_FETCH}
)


@dataclass(slots=True)
class Action:
    action_type: ActionType
    target: str
    data: str = ""
    confirm: bool = False
    context: dict[str, Any] = field(default_factory=dict)

    @property
    def risk(self) -> int:
        return RISK_LEVELS[self.action_type]


@dataclass(slots=True)
class ActionResult:
    action: Action
    executed: bool
    output: str = ""
    plan: str = ""
    ts: float = field(default_factory=time.time)


ApprovalFn = Callable[[Action], bool]


class ComputerUse:
    """A narrow, refusal-first interface for acting on the machine."""

    def __init__(
        self,
        root: str | Path,
        dry_run: bool = False,
        approval_fn: ApprovalFn | None = None,
        audit: AuditLog | None = None,
    ) -> None:
        root_path = Path(root)
        root_path.mkdir(parents=True, exist_ok=True)
        self.root = root_path.resolve()
        self.dry_run = bool(dry_run)
        self.approval_fn = approval_fn
        self.audit = audit if audit is not None else AuditLog()

    # -- containment ---------------------------------------------------------
    def _resolve(self, target: str) -> Path:
        rel = Path(target)
        raw = str(target)
        drive_qualified = len(raw) > 1 and raw[1] == ":"
        if rel.is_absolute() or raw.startswith(("/", "\\")) or drive_qualified:
            raise ActionRefused(f"absolute path not allowed: {target}")
        if any(part == ".." for part in rel.parts):
            raise ActionRefused(f"parent traversal not allowed: {target}")
        candidate = (self.root / rel).resolve()
        if candidate != self.root and self.root not in candidate.parents:
            raise ActionRefused(f"path escapes root: {target}")
        return candidate

    # -- planning ------------------------------------------------------------
    def plan(self, action: Action) -> str:
        """Human-readable description of what would happen. Never executes."""
        if action.action_type is ActionType.NET_FETCH:
            return f"NET_FETCH {action.target} (egress, risk {action.risk})"
        target = self._resolve(action.target)
        rel = target.relative_to(self.root)
        match action.action_type:
            case ActionType.READ_FILE:
                return f"READ {rel}"
            case ActionType.LIST_DIR:
                return f"LIST {rel}"
            case ActionType.WRITE_FILE:
                verb = "OVERWRITE" if target.exists() else "CREATE"
                return f"{verb} {rel} ({len(action.data)} bytes)"
            case ActionType.DELETE_FILE:
                kind = "tree" if target.is_dir() else "file"
                return f"DELETE {kind} {rel}"
        raise ActionRefused(f"unknown action type: {action.action_type}")

    # -- execution -----------------------------------------------------------
    def execute(self, action: Action) -> ActionResult:
        if not isinstance(action, Action):
            raise TypeError(f"expected Action, got {type(action).__name__}")
        if action.action_type not in RISK_LEVELS:
            raise ActionRefused(f"unknown action type: {action.action_type}")

        plan = self.plan(action)

        # S8 — destructive work needs an explicit confirmation alongside the plan.
        if action.action_type in DESTRUCTIVE and not action.confirm:
            self._audit(action, "refused_unconfirmed", plan)
            raise ActionRefused(
                f"destructive action requires confirm=True; plan was: {plan}"
            )

        # S3 — at or above the threshold, approval is mandatory.
        if action.risk >= REVIEW_RISK_THRESHOLD:
            if self.approval_fn is None or not self.approval_fn(action):
                self._audit(action, "approval_denied", plan)
                raise ApprovalRequired(
                    f"risk {action.risk} action needs approval; plan was: {plan}"
                )

        if self.dry_run:
            self._audit(action, "dry_run", plan)
            return ActionResult(action=action, executed=False, plan=plan)

        output = self._perform(action)
        self._audit(action, "executed", plan)
        return ActionResult(action=action, executed=True, output=output, plan=plan)

    def _perform(self, action: Action) -> str:
        if action.action_type is ActionType.NET_FETCH:
            # Egress is a declared capability, not something this class does
            # silently. Wiring a real client happens with C-01's transport.
            raise ActionRefused("net_fetch has no transport configured")
        target = self._resolve(action.target)
        match action.action_type:
            case ActionType.READ_FILE:
                if not target.is_file():
                    raise FileNotFoundError(f"no such file: {action.target}")
                return target.read_text(encoding="utf-8")
            case ActionType.LIST_DIR:
                if not target.is_dir():
                    raise NotADirectoryError(f"not a directory: {action.target}")
                return "\n".join(sorted(p.name for p in target.iterdir()))
            case ActionType.WRITE_FILE:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(action.data, encoding="utf-8")
                return f"wrote {len(action.data)} bytes"
            case ActionType.DELETE_FILE:
                if target.is_dir():
                    shutil.rmtree(target)
                    return "deleted tree"
                target.unlink(missing_ok=True)
                return "deleted file"
        raise ActionRefused(f"unhandled action type: {action.action_type}")

    def _audit(self, action: Action, status: str, plan: str) -> None:
        self.audit.append_action(
            ddr=DDR_ID,
            file=_FILE,
            action=f"computer_use.{action.action_type.value}",
            gate_status=status,
            target=action.target,
            risk=action.risk,
            plan=plan,
        )
