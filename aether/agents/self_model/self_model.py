"""B-15 — self-model maintenance.

The system's model of itself: what it can do, what it has failed at, and how
confident it is in each claim.

The one rule that makes this useful rather than flattering: **a capability claim
must be evidenced.** ``claim_capability`` cross-checks the lockbox — a capability
nobody granted cannot be claimed, no matter what the model says about itself.
An unevidenced self-model is worse than none, because every downstream consumer
(C-05 boundary mapper, D-01 proposals, D-02 planner) treats it as ground truth
when deciding what to attempt.

Confidence moves on evidence too: successes and failures are recorded and the
score is derived, never set directly.
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "CapabilityClaim",
    "KnownLimit",
    "SelfModel",
    "UnevidencedClaim",
]

DDR_ID = "B-15"
_FILE = "aether/agents/self_model/self_model.py"

DEFAULT_STORE = Path(__file__).resolve().parent / "self_model.jsonl"

#: Claims need this many observations before confidence is treated as meaningful.
MIN_OBSERVATIONS = 3


class UnevidencedClaim(PermissionError):
    """A capability was claimed that the lockbox does not grant."""


@dataclass(slots=True)
class CapabilityClaim:
    name: str
    ddr_id: str
    capability: str
    successes: int = 0
    failures: int = 0
    last_used_ts: float = 0.0

    @property
    def observations(self) -> int:
        return self.successes + self.failures

    @property
    def confidence(self) -> float:
        """Derived, never assigned. Unobserved claims sit at 0.5 (unknown)."""
        if self.observations == 0:
            return 0.5
        return self.successes / self.observations

    @property
    def established(self) -> bool:
        return self.observations >= MIN_OBSERVATIONS


@dataclass(slots=True)
class KnownLimit:
    """Something the system has demonstrably failed at."""

    description: str
    evidence: str
    ts: float = field(default_factory=time.time)


class SelfModel:
    """What the system believes about itself, kept honest by the lockbox."""

    def __init__(
        self,
        lockbox: Lockbox,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
    ) -> None:
        self.lockbox = lockbox
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self._claims: dict[str, CapabilityClaim] = {}
        self._limits: list[KnownLimit] = []
        self._replay()

    def _replay(self) -> None:
        if not self.store_path.is_file():
            return
        with self.store_path.open("r", encoding="utf-8") as fh:
            for raw in fh:
                stripped = raw.strip()
                if not stripped:
                    continue
                try:
                    rec = json.loads(stripped)
                except json.JSONDecodeError as exc:
                    raise ValueError(f"corrupt self-model: {self.store_path}") from exc
                if rec.get("kind") == "claim":
                    claim = CapabilityClaim(
                        name=rec["name"], ddr_id=rec["ddr_id"],
                        capability=rec["capability"],
                        successes=rec.get("successes", 0),
                        failures=rec.get("failures", 0),
                        last_used_ts=rec.get("last_used_ts", 0.0),
                    )
                    self._claims[claim.name] = claim
                elif rec.get("kind") == "limit":
                    self._limits.append(
                        KnownLimit(
                            description=rec["description"],
                            evidence=rec.get("evidence", ""),
                            ts=rec.get("ts", 0.0),
                        )
                    )

    def _persist(self, kind: str, payload: dict[str, Any]) -> None:
        record = {"kind": kind, **payload}
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n")

    # -- capability claims ---------------------------------------------------
    def claim_capability(self, name: str, ddr_id: str, capability: str) -> CapabilityClaim:
        """Record that the system can do something — if the lockbox agrees.

        This is the honesty gate. Without it the self-model degenerates into
        whatever the system would like to be true about itself.
        """
        if not self.lockbox.check(ddr_id, capability):
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="self_model.unevidenced_claim",
                gate_status="refused", claim=name, capability=capability,
            )
            raise UnevidencedClaim(
                f"{ddr_id} does not hold {capability}; claim {name!r} refused"
            )
        claim = self._claims.get(name)
        if claim is None:
            claim = CapabilityClaim(name=name, ddr_id=ddr_id, capability=capability)
            self._claims[name] = claim
            self._persist("claim", asdict(claim))
        return claim

    def record_outcome(self, name: str, success: bool) -> CapabilityClaim:
        """Move confidence by evidence."""
        claim = self._claims.get(name)
        if claim is None:
            raise KeyError(f"unknown capability claim: {name}")
        if success:
            claim.successes += 1
        else:
            claim.failures += 1
        claim.last_used_ts = time.time()
        self._persist("claim", asdict(claim))
        return claim

    def confidence(self, name: str) -> float:
        claim = self._claims.get(name)
        return claim.confidence if claim is not None else 0.0

    def can(self, name: str, threshold: float = 0.6) -> bool:
        """Should the system attempt this?

        Requires an established track record: an unobserved claim sits at 0.5
        and must not read as a yes.
        """
        claim = self._claims.get(name)
        if claim is None:
            return False
        return claim.established and claim.confidence >= threshold

    # -- limits --------------------------------------------------------------
    def record_limit(self, description: str, evidence: str) -> KnownLimit:
        if not evidence:
            raise ValueError("a limit must cite evidence")
        limit = KnownLimit(description=description, evidence=evidence)
        self._limits.append(limit)
        self._persist("limit", asdict(limit))
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="self_model.limit",
            gate_status="recorded", description=description,
        )
        return limit

    @property
    def limits(self) -> tuple[KnownLimit, ...]:
        return tuple(self._limits)

    @property
    def claims(self) -> tuple[CapabilityClaim, ...]:
        return tuple(sorted(self._claims.values(), key=lambda c: c.name))

    def summary(self) -> dict[str, Any]:
        return {
            "claims": {c.name: round(c.confidence, 3) for c in self.claims},
            "established": [c.name for c in self.claims if c.established],
            "limits": [limit.description for limit in self._limits],
        }
