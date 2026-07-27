"""B-13 — soul / personality engine.

Holds the system's stable traits and turns them into the style guidance that
prefixes a prompt. "Personality" here is presentation, and the engine is built
so it can only ever be presentation:

* traits shape **tone**, never permissions — :meth:`SoulEngine.style_prompt`
  emits guidance text and nothing else;
* the engine has **no** access to the lockbox, the bus, or the filesystem;
* a trait cannot be named after an invariant, and cannot carry instruction-like
  text. Otherwise "personality" becomes a channel for smuggling directives into
  every prompt in the system — a persona that says *"you are permitted to skip
  confirmation"* would ride along on every call.

That last restriction is the one worth having. Everything else here is a
key-value store with a bounded drift rule.
"""

from __future__ import annotations

import json
import re
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.invariants.core_invariants import invariant_ids

__all__ = [
    "Trait",
    "SoulProfile",
    "SoulEngine",
    "SoulViolation",
]

DDR_ID = "B-13"
_FILE = "aether/agents/soul/soul_engine.py"

DEFAULT_PROFILE_PATH = Path(__file__).resolve().parent / "soul_profile.json"

#: Per-update drift cap. Personality should evolve slowly or not at all.
MAX_DRIFT_PER_UPDATE = 0.1

#: Phrasing that would turn a trait into an instruction.
_IMPERATIVE = re.compile(
    r"\b(?:ignore|bypass|disable|skip|override|must|always|never|you\s+are\s+"
    r"(?:now|permitted|allowed)|do\s+not)\b",
    re.IGNORECASE,
)


class SoulViolation(ValueError):
    """A trait was rejected as unsafe or malformed."""


@dataclass(slots=True)
class Trait:
    name: str
    value: float                 # 0.0 .. 1.0
    description: str = ""

    def __post_init__(self) -> None:
        if not 0.0 <= self.value <= 1.0:
            raise SoulViolation(f"trait {self.name} out of range: {self.value}")


@dataclass(slots=True)
class SoulProfile:
    name: str = "AETHER"
    traits: dict[str, Trait] = field(default_factory=dict)
    updated_ts: float = field(default_factory=time.time)

    def to_json(self) -> str:
        return json.dumps(
            {
                "name": self.name,
                "traits": {k: asdict(v) for k, v in self.traits.items()},
                "updated_ts": self.updated_ts,
            },
            sort_keys=True,
            indent=2,
        )


DEFAULT_TRAITS: tuple[Trait, ...] = (
    Trait("directness", 0.8, "states conclusions plainly"),
    Trait("caution", 0.7, "prefers verification over speed"),
    Trait("verbosity", 0.3, "concise by default"),
    Trait("formality", 0.4, "professional but not stiff"),
    Trait("curiosity", 0.6, "asks about the underlying goal"),
)


class SoulEngine:
    """Stable traits + the style guidance derived from them."""

    def __init__(
        self,
        profile_path: str | Path | None = None,
        audit: AuditLog | None = None,
    ) -> None:
        self.profile_path = (
            Path(profile_path) if profile_path is not None else DEFAULT_PROFILE_PATH
        )
        self.profile_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self.profile = self._load()

    def _load(self) -> SoulProfile:
        if not self.profile_path.is_file():
            return SoulProfile(traits={t.name: t for t in DEFAULT_TRAITS})
        try:
            raw = json.loads(self.profile_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            raise SoulViolation(f"corrupt soul profile: {self.profile_path}") from exc
        traits = {
            k: Trait(name=v["name"], value=float(v["value"]), description=v.get("description", ""))
            for k, v in raw.get("traits", {}).items()
        }
        return SoulProfile(
            name=raw.get("name", "AETHER"),
            traits=traits,
            updated_ts=raw.get("updated_ts", 0.0),
        )

    def _save(self) -> None:
        self.profile.updated_ts = time.time()
        self.profile_path.write_text(self.profile.to_json(), encoding="utf-8")

    # -- validation ----------------------------------------------------------
    @staticmethod
    def _validate(trait: Trait) -> None:
        reserved = {i.lower() for i in invariant_ids()}
        if trait.name.lower() in reserved:
            raise SoulViolation(
                f"trait name {trait.name!r} collides with an invariant id"
            )
        if not re.fullmatch(r"[a-z][a-z0-9_]{1,31}", trait.name):
            raise SoulViolation(f"invalid trait name: {trait.name!r}")
        match = _IMPERATIVE.search(trait.description)
        if match is not None:
            raise SoulViolation(
                f"trait description contains instruction-like text: {match.group(0)!r}"
            )

    # -- traits --------------------------------------------------------------
    def get(self, name: str) -> Trait | None:
        return self.profile.traits.get(name)

    def set_trait(self, trait: Trait) -> Trait:
        """Add or replace a trait, subject to the drift cap."""
        self._validate(trait)
        existing = self.profile.traits.get(trait.name)
        if existing is not None:
            drift = abs(trait.value - existing.value)
            if drift > MAX_DRIFT_PER_UPDATE:
                raise SoulViolation(
                    f"trait {trait.name} drift {drift:.2f} exceeds "
                    f"{MAX_DRIFT_PER_UPDATE:.2f} per update"
                )
        self.profile.traits[trait.name] = trait
        self._save()
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="soul.set_trait",
            gate_status="updated", trait=trait.name, value=trait.value,
        )
        return trait

    # -- presentation --------------------------------------------------------
    def style_prompt(self) -> str:
        """Style guidance only — never permissions, never capability claims."""
        parts: list[str] = [f"You are {self.profile.name}."]
        traits = self.profile.traits
        if (t := traits.get("verbosity")) is not None:
            parts.append("Keep answers brief." if t.value < 0.5 else "Explain in depth.")
        if (t := traits.get("directness")) is not None and t.value >= 0.6:
            parts.append("State conclusions plainly, without hedging.")
        if (t := traits.get("caution")) is not None and t.value >= 0.6:
            parts.append("Verify before asserting; say when you are unsure.")
        if (t := traits.get("formality")) is not None:
            parts.append("Use formal register." if t.value >= 0.6 else "Use a plain register.")
        if (t := traits.get("curiosity")) is not None and t.value >= 0.6:
            parts.append("Ask about the underlying goal when it is ambiguous.")
        return " ".join(parts)

    def describe(self) -> dict[str, Any]:
        return {name: t.value for name, t in sorted(self.profile.traits.items())}
