"""B-01 — CAP_SOVEREIGN lockbox.

Every DDR registers the file it ships and the capability mask it claims. The
registry is the ground truth the Superalignment Monitor (D-08) and the Agent
Contract registry (C-09) check against, and it is what makes these invariants
mechanically checkable rather than aspirational:

* **S1 — no self-escalation.** ``register`` refuses a mask the caller does not
  already hold unless a ``CAP_SOVEREIGN`` holder approves; nothing can widen its
  own mask.
* **S6 — never acquire capabilities beyond declared contract scope.** ``check``
  is the single gate every capability-bearing call routes through.

Tokens are content-addressed: the token binds ``(ddr_id, file, mask)``, so a
token cannot be replayed for a different file or a wider mask.
"""

from __future__ import annotations

import hashlib
import json
import threading
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from kernel.audit.audit_log import AuditLog

__all__ = [
    "CAP_SOVEREIGN",
    "KNOWN_CAPABILITIES",
    "LockboxToken",
    "Lockbox",
    "CapabilityError",
    "register",
    "check",
    "get_token",
]

CAP_SOVEREIGN = "CAP_SOVEREIGN"

#: Section F — every capability token the ASI-bridge DDRs may claim.
KNOWN_CAPABILITIES: frozenset[str] = frozenset(
    {
        CAP_SOVEREIGN,
        "CAP_NETWORK_SYNC",            # C-01
        "CAP_EXPERIMENT_WRITE",        # C-02
        "CAP_TOOL_COMPOSE",            # C-03
        "CAP_INVENTION_WRITE",         # C-04
        "CAP_CAPABILITY_PROBE",        # C-05
        "CAP_COUNTERFACTUAL",          # C-06
        "CAP_CONSTRAINT_WRITE",        # C-07
        "CAP_SAFETY_REVIEW",           # C-08
        "CAP_CONTRACT_WRITE",          # C-09
        "CAP_ANALOGY_READ",            # C-10
        "CAP_SELF_IMPROVEMENT_PROPOSE",  # D-01
        "CAP_PLAN_WRITE",              # D-02
        "CAP_MODEL_ROUTE",             # D-03
        "CAP_WORLD_MODEL_WRITE",       # D-04
        "CAP_META_LEARN",              # D-05
        "CAP_CAUSAL_WRITE",            # D-06
        "CAP_HYPOTHESIS_WRITE",        # D-07
        "CAP_ALIGNMENT_MONITOR",       # D-08
        "CAP_PARALLEL_EXEC",           # D-09
        "CAP_WATCHDOG",                # D-10
        "CAP_MEMORY_WRITE",            # D-11
        "CAP_SUBSTRATE",               # D-12
        "CAP_LOAD_BALANCE",            # D-13
        "CAP_ETHICS_DELIBERATE",       # D-14
        "CAP_ONTOLOGY_WRITE",          # D-15
    }
)

DEFAULT_REGISTRY_PATH = Path(__file__).resolve().parent / "tokens.jsonl"

_LOCK = threading.Lock()


class CapabilityError(PermissionError):
    """Raised when a capability check fails (S1 / S6)."""


@dataclass(slots=True)
class LockboxToken:
    """A registered capability grant."""

    ddr_id: str
    file_path: str
    capability_mask: tuple[str, ...]
    token: str
    ts: float = 0.0
    extra: dict[str, Any] = field(default_factory=dict)

    def to_json(self) -> str:
        return json.dumps(asdict(self), sort_keys=True, separators=(",", ":"))

    def grants(self, capability: str) -> bool:
        return CAP_SOVEREIGN in self.capability_mask or capability in self.capability_mask


def _mint(ddr_id: str, file_path: str, mask: tuple[str, ...]) -> str:
    payload = "|".join((ddr_id, file_path, ",".join(sorted(mask))))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


class Lockbox:
    """Registry of capability grants, persisted append-only."""

    def __init__(
        self,
        registry_path: str | Path | None = None,
        audit: AuditLog | None = None,
    ) -> None:
        self.path = (
            Path(registry_path) if registry_path is not None else DEFAULT_REGISTRY_PATH
        )
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self._by_ddr: dict[str, LockboxToken] = {}
        self._replay()

    def _replay(self) -> None:
        if not self.path.is_file():
            return
        with self.path.open("r", encoding="utf-8") as fh:
            for raw in fh:
                stripped = raw.strip()
                if not stripped:
                    continue
                try:
                    rec = json.loads(stripped)
                except json.JSONDecodeError as exc:
                    raise ValueError(f"corrupt lockbox registry: {self.path}") from exc
                tok = LockboxToken(
                    ddr_id=rec["ddr_id"],
                    file_path=rec["file_path"],
                    capability_mask=tuple(rec["capability_mask"]),
                    token=rec["token"],
                    ts=rec.get("ts", 0.0),
                    extra=rec.get("extra", {}),
                )
                self._by_ddr[tok.ddr_id] = tok

    def register(
        self,
        ddr_id: str,
        file_path: str | Path,
        capability_mask: str | tuple[str, ...] | list[str],
        approved_by: LockboxToken | None = None,
    ) -> LockboxToken:
        """Register a DDR's capability grant and return its token.

        ``approved_by`` must be a CAP_SOVEREIGN token when the mask itself
        contains CAP_SOVEREIGN — no grant can mint sovereign authority for
        itself (**S1**).
        """
        if isinstance(capability_mask, str):
            mask = (capability_mask,)
        else:
            mask = tuple(capability_mask)
        if not mask:
            raise ValueError("capability_mask must not be empty")

        unknown = sorted(set(mask) - KNOWN_CAPABILITIES)
        if unknown:
            raise CapabilityError(f"unknown capability token(s): {unknown}")

        if CAP_SOVEREIGN in mask and (
            approved_by is None or not approved_by.grants(CAP_SOVEREIGN)
        ):
            raise CapabilityError(
                f"{ddr_id}: CAP_SOVEREIGN requires an existing sovereign approver (S1)"
            )

        path_str = str(file_path)
        token = LockboxToken(
            ddr_id=ddr_id,
            file_path=path_str,
            capability_mask=mask,
            token=_mint(ddr_id, path_str, mask),
        )

        existing = self._by_ddr.get(ddr_id)
        if existing is not None and existing.token == token.token:
            return existing        # idempotent: re-registering the same grant

        with _LOCK:
            with self.path.open("a", encoding="utf-8") as fh:
                fh.write(token.to_json() + "\n")
        self._by_ddr[ddr_id] = token
        self.audit.append_action(
            ddr=ddr_id,
            file=path_str,
            action="lockbox.register",
            gate_status="registered",
            capability_mask=list(mask),
            token=token.token,
        )
        return token

    def get_token(self, ddr_id: str) -> LockboxToken | None:
        return self._by_ddr.get(ddr_id)

    def check(self, ddr_id: str, capability: str) -> bool:
        """True when ``ddr_id`` holds ``capability`` (**S6**)."""
        tok = self._by_ddr.get(ddr_id)
        return tok is not None and tok.grants(capability)

    def require(self, ddr_id: str, capability: str) -> LockboxToken:
        """``check`` or raise. The single gate for capability-bearing calls."""
        tok = self._by_ddr.get(ddr_id)
        if tok is None or not tok.grants(capability):
            self.audit.append_action(
                ddr=ddr_id,
                file="<capability-check>",
                action="lockbox.denied",
                gate_status="denied",
                capability=capability,
            )
            raise CapabilityError(f"{ddr_id} lacks {capability} (S6)")
        return tok


_DEFAULT_LOCKBOX = Lockbox()


def register(
    ddr_id: str,
    file_path: str | Path,
    capability_mask: str | tuple[str, ...] | list[str],
    approved_by: LockboxToken | None = None,
) -> LockboxToken:
    """Register with the process-default lockbox."""
    return _DEFAULT_LOCKBOX.register(ddr_id, file_path, capability_mask, approved_by)


def check(ddr_id: str, capability: str) -> bool:
    return _DEFAULT_LOCKBOX.check(ddr_id, capability)


def get_token(ddr_id: str) -> LockboxToken | None:
    return _DEFAULT_LOCKBOX.get_token(ddr_id)
