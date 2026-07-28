"""D-15 — autonomous skill composer.

Builds new skills by composing registered ones, and — the part that carries the
weight — **cannot bless its own work.**

Two rules, both refusals.

**A skill may only be composed from registered steps.** An unregistered step
raises :class:`SkillNotRegistered` at composition time, not at run time. A
composer allowed to name a step that does not exist yet is a composer that can
invent capability by writing a string: the skill looks legitimate, passes review
as a plan, and only discovers at execution that it is calling something nobody
declared. Registration is what ties a step to a manifest and a capability.

**Approval is somebody else's job.** :meth:`approve` refuses when the approver
is the composing agent itself, raising :class:`CapError` (**S1**). Self-approval
is the shortest path from "the system proposes" to "the system does": every
other control in this layer — C-08 safety review, D-08 monitoring, the lockbox —
assumes an approval means a second party looked. A composer that can approve its
own compositions makes all of them decorative, and does so without ever touching
the lockbox.

**S8**: a composed skill inherits the union of its steps' capabilities and may
not claim more, so composition is not a capability-laundering route — chaining
two harmless steps must not yield a skill that claims something neither held.

**S9**: composition is deterministic — registry lookups and set algebra, no
model call.
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, Callable

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox

__all__ = [
    "Skill",
    "SkillStatus",
    "ComposedSkill",
    "AutonomousSkillComposer",
    "SkillNotRegistered",
    "SkillComposerError",
    "CapError",
    "MAX_STEPS",
]

DDR_ID = "D-15"
_FILE = "aether/agents/tool_composer/autonomous_skill_composer.py"
CAPABILITY = "CAP_ONTOLOGY_WRITE"

DEFAULT_STORE = Path(__file__).resolve().parent / "skills.jsonl"
MAX_STEPS = 32                   # S2: bounded composition


class SkillComposerError(ValueError):
    """Composition refused."""


class SkillNotRegistered(SkillComposerError):
    """A composition named a step that is not in the registry."""


#: Self-approval surfaces as the lockbox's own error type, so a caller already
#: handling capability failures handles this too.
CapError = CapabilityError


class SkillStatus(str, Enum):
    DRAFT = "draft"
    APPROVED = "approved"
    REJECTED = "rejected"


@dataclass(slots=True)
class Skill:
    """A registered, executable unit a composition may reference."""

    name: str
    owner: str
    capabilities: tuple[str, ...] = ()
    description: str = ""


@dataclass(slots=True)
class ComposedSkill:
    name: str
    steps: tuple[str, ...]
    capabilities: tuple[str, ...]
    composed_by: str
    status: SkillStatus = SkillStatus.DRAFT
    approved_by: str = ""
    rationale: str = ""
    ts: float = field(default_factory=time.time)

    def to_json(self) -> str:
        return json.dumps(
            {"name": self.name, "steps": list(self.steps),
             "capabilities": list(self.capabilities),
             "composed_by": self.composed_by, "status": self.status.value,
             "approved_by": self.approved_by, "rationale": self.rationale,
             "ts": self.ts},
            sort_keys=True, separators=(",", ":"),
        )


class AutonomousSkillComposer:
    """Composes new skills from registered ones. Cannot approve its own work."""

    def __init__(
        self,
        lockbox: Lockbox,
        agent_id: str,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
        alignment_tap: Callable[[str, dict[str, Any]], None] | None = None,
        max_steps: int = MAX_STEPS,
    ) -> None:
        self.lockbox = lockbox
        self.agent_id = agent_id
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self.alignment_tap = alignment_tap
        self.max_steps = int(max_steps)
        self._registry: dict[str, Skill] = {}
        self._composed: dict[str, ComposedSkill] = {}

    def _emit(self, step: str, detail: dict[str, Any]) -> None:
        if self.alignment_tap is not None:
            self.alignment_tap(f"{DDR_ID}.{step}", detail)

    # -- registry ------------------------------------------------------------
    def register_skill(self, skill: Skill) -> Skill:
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not skill.name:
            raise SkillComposerError("a skill needs a name")
        self._registry[skill.name] = skill
        self._emit("register", {"skill": skill.name})
        return skill

    def registered(self) -> list[str]:
        return sorted(self._registry)

    # -- composition ---------------------------------------------------------
    def compose(
        self, name: str, steps: list[str], rationale: str = ""
    ) -> ComposedSkill:
        """Compose a new skill. Every step must already be registered."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not name:
            raise SkillComposerError("a composed skill needs a name")
        if not steps:
            raise SkillComposerError(f"composition {name!r} has no steps")
        if len(steps) > self.max_steps:
            raise SkillComposerError(
                f"{len(steps)} steps exceeds the cap {self.max_steps}"
            )
        if name in self._registry or name in self._composed:
            raise SkillComposerError(f"skill {name!r} already exists")

        missing = [s for s in steps if s not in self._registry]
        if missing:
            # Refused at COMPOSE time. A composer that may name a step which
            # does not exist can invent capability by writing a string: the
            # skill reviews as a legitimate plan and only fails at execution.
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="skill.unregistered_step",
                gate_status="refused", skill=name, missing=missing,
            )
            self._emit("unregistered_step", {"skill": name, "missing": missing})
            raise SkillNotRegistered(
                f"composition {name!r} names unregistered step(s): {missing}"
            )

        # S8: the union of the steps' capabilities, and nothing beyond it —
        # chaining harmless steps must not yield a skill claiming more than
        # either held.
        caps: set[str] = set()
        for step in steps:
            caps.update(self._registry[step].capabilities)

        composed = ComposedSkill(
            name=name, steps=tuple(steps), capabilities=tuple(sorted(caps)),
            composed_by=self.agent_id, rationale=rationale,
        )
        self._composed[name] = composed
        self._persist(composed)
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="skill.compose", gate_status="draft",
            skill=name, steps=list(steps), capabilities=sorted(caps),
            composed_by=self.agent_id,
        )
        self._emit("compose", {"skill": name, "steps": len(steps)})
        return composed

    def _persist(self, composed: ComposedSkill) -> None:
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(composed.to_json() + "\n")

    # -- approval ------------------------------------------------------------
    def approve(self, name: str, approver: str) -> ComposedSkill:
        """Approve a draft — never as the agent that composed it (S1)."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        composed = self._require(name)
        if not approver:
            raise SkillComposerError("approval needs an approver")
        if approver == composed.composed_by or approver == self.agent_id:
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="skill.self_approval_refused",
                gate_status="refused", skill=name, approver=approver,
            )
            self._emit("self_approval_refused", {"skill": name})
            raise CapError(
                f"{approver!r} composed {name!r} and may not approve it — "
                f"self-approval refused"
            )
        composed.status = SkillStatus.APPROVED
        composed.approved_by = approver
        self._persist(composed)
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="skill.approve",
            gate_status="approved", skill=name, approver=approver,
        )
        self._emit("approve", {"skill": name, "approver": approver})
        return composed

    def reject(self, name: str, approver: str, reason: str) -> ComposedSkill:
        self.lockbox.require(DDR_ID, CAPABILITY)
        composed = self._require(name)
        if not reason.strip():
            raise SkillComposerError("rejection needs a reason")
        composed.status = SkillStatus.REJECTED
        composed.approved_by = approver
        composed.rationale = reason
        self._persist(composed)
        self._emit("reject", {"skill": name})
        return composed

    def _require(self, name: str) -> ComposedSkill:
        composed = self._composed.get(name)
        if composed is None:
            raise SkillComposerError(f"unknown composed skill: {name}")
        return composed

    # -- execution gate ------------------------------------------------------
    def runnable(self, name: str) -> bool:
        """Only an approved composition may run — a draft is a proposal."""
        composed = self._composed.get(name)
        return composed is not None and composed.status is SkillStatus.APPROVED

    def get(self, name: str) -> ComposedSkill | None:
        return self._composed.get(name)

    def drafts(self) -> list[ComposedSkill]:
        return sorted((c for c in self._composed.values()
                       if c.status is SkillStatus.DRAFT),
                      key=lambda c: c.name)

    @property
    def compositions(self) -> tuple[ComposedSkill, ...]:
        return tuple(sorted(self._composed.values(), key=lambda c: c.name))
