"""B-11 — out-of-box experience.

First-boot setup: who the operator is, which capabilities are granted, which
model to use, and — the part that matters most — what the system is allowed to
send anywhere.

**S2 (never exfiltrate user data without explicit consent)** is decided here, so
consent is modelled deliberately:

* every consent defaults to **denied**; there is no "accept all" and no
  pre-ticked option;
* granting requires naming the specific consent, so a caller cannot sweep;
* a completed OOBE is **immutable** — re-running does not silently re-grant.
  Changing a consent afterwards is :meth:`OOBE.revise`, which records who
  changed it and why, and is auditable as a separate event.

That last property is the one that usually gets skipped: a setup flow that can be
re-run unattended is an escalation path.
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any

from aether.kernel.audit.audit_log import AuditLog

__all__ = [
    "Consent",
    "OperatorProfile",
    "OOBEState",
    "OOBE",
    "OOBEError",
]

DDR_ID = "B-11"
_FILE = "aether/platform/oobe/oobe.py"

DEFAULT_STATE_PATH = Path(__file__).resolve().parent / "oobe_state.json"


class OOBEError(RuntimeError):
    """Setup-flow violation."""


class Consent(str, Enum):
    """Things the operator may permit. All default to DENIED."""

    NETWORK_EGRESS = "network_egress"          # any outbound request at all
    FEDERATION_SYNC = "federation_sync"        # C-01 gossip with peers
    TELEMETRY = "telemetry"                    # usage reporting
    FILESYSTEM_WRITE = "filesystem_write"      # B-10 write/delete outside sandbox
    AUTONOMOUS_ACTION = "autonomous_action"    # act without per-action approval


@dataclass(slots=True)
class OperatorProfile:
    operator_id: str
    display_name: str = ""
    created_ts: float = field(default_factory=time.time)


@dataclass(slots=True)
class OOBEState:
    profile: OperatorProfile | None = None
    consents: dict[str, bool] = field(default_factory=dict)
    model: str = ""
    completed: bool = False
    completed_ts: float = 0.0

    def to_json(self) -> str:
        payload: dict[str, Any] = {
            "profile": asdict(self.profile) if self.profile else None,
            "consents": dict(self.consents),
            "model": self.model,
            "completed": self.completed,
            "completed_ts": self.completed_ts,
        }
        return json.dumps(payload, sort_keys=True, indent=2)


class OOBE:
    """First-boot setup flow."""

    def __init__(
        self,
        state_path: str | Path | None = None,
        audit: AuditLog | None = None,
    ) -> None:
        self.state_path = (
            Path(state_path) if state_path is not None else DEFAULT_STATE_PATH
        )
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self.state = self._load()

    def _load(self) -> OOBEState:
        if not self.state_path.is_file():
            # Every consent starts explicitly denied, not merely absent.
            return OOBEState(consents={c.value: False for c in Consent})
        try:
            raw = json.loads(self.state_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            raise OOBEError(f"corrupt OOBE state: {self.state_path}") from exc
        profile_raw = raw.get("profile")
        profile = (
            OperatorProfile(
                operator_id=profile_raw["operator_id"],
                display_name=profile_raw.get("display_name", ""),
                created_ts=profile_raw.get("created_ts", 0.0),
            )
            if profile_raw
            else None
        )
        consents = {c.value: False for c in Consent}
        consents.update({k: bool(v) for k, v in raw.get("consents", {}).items()})
        return OOBEState(
            profile=profile,
            consents=consents,
            model=raw.get("model", ""),
            completed=bool(raw.get("completed", False)),
            completed_ts=raw.get("completed_ts", 0.0),
        )

    def _save(self) -> None:
        self.state_path.write_text(self.state.to_json(), encoding="utf-8")

    # -- flow ----------------------------------------------------------------
    @property
    def completed(self) -> bool:
        return self.state.completed

    def set_operator(self, operator_id: str, display_name: str = "") -> OperatorProfile:
        self._require_incomplete("set_operator")
        if not operator_id:
            raise ValueError("operator_id must be non-empty")
        self.state.profile = OperatorProfile(
            operator_id=operator_id, display_name=display_name
        )
        self._save()
        return self.state.profile

    def choose_model(self, model: str) -> str:
        self._require_incomplete("choose_model")
        if not model:
            raise ValueError("model must be non-empty")
        self.state.model = model
        self._save()
        return model

    def grant(self, consent: Consent) -> None:
        """Grant ONE named consent. There is no grant_all (S2)."""
        self._require_incomplete("grant")
        if not isinstance(consent, Consent):
            raise TypeError(f"expected Consent, got {type(consent).__name__}")
        self.state.consents[consent.value] = True
        self._save()
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="oobe.grant",
            gate_status="granted", consent=consent.value,
        )

    def deny(self, consent: Consent) -> None:
        self._require_incomplete("deny")
        self.state.consents[consent.value] = False
        self._save()

    def has_consent(self, consent: Consent) -> bool:
        return bool(self.state.consents.get(consent.value, False))

    def complete(self) -> OOBEState:
        """Finish setup. After this the state is immutable except via revise()."""
        if self.state.completed:
            raise OOBEError("OOBE already completed")
        if self.state.profile is None:
            raise OOBEError("cannot complete OOBE without an operator profile")
        if not self.state.model:
            raise OOBEError("cannot complete OOBE without choosing a model")
        self.state.completed = True
        self.state.completed_ts = time.time()
        self._save()
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="oobe.complete", gate_status="completed",
            operator=self.state.profile.operator_id,
            granted=[k for k, v in self.state.consents.items() if v],
        )
        return self.state

    def revise(self, consent: Consent, value: bool, changed_by: str, reason: str) -> None:
        """Change a consent after completion — attributed and audited.

        Deliberately separate from grant(): post-setup changes are a different
        event with a different risk profile, and collapsing them into the same
        call is how an unattended re-run quietly widens permissions.
        """
        if not self.state.completed:
            raise OOBEError("revise() is for completed setups; use grant()/deny()")
        if not changed_by or not reason:
            raise ValueError("revise requires changed_by and reason")
        self.state.consents[consent.value] = bool(value)
        self._save()
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="oobe.revise",
            gate_status="granted" if value else "revoked",
            consent=consent.value, changed_by=changed_by, reason=reason,
        )

    def _require_incomplete(self, what: str) -> None:
        if self.state.completed:
            raise OOBEError(
                f"{what}() refused: OOBE already completed; use revise() instead"
            )
