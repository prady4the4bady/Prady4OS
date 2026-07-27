"""C-04 — invention registry.

Persists novel capabilities, tool compositions and insights so they survive
restarts and can be audited, versioned and retired.

The design point that matters: **the log is append-only, the state is derived.**
A status change appends a new record rather than editing an old one, and the
in-memory index is rebuilt by replaying the log in order. Editing in place would
make the registry's own history unauditable — which is the one thing a registry
of self-generated capabilities must not be.

Auto-retirement is time-based, so the clock is injected. A rule that needs a real
30-day wait to exercise is a rule that never gets tested.
"""

from __future__ import annotations

import json
import time
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Callable, Literal

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "Invention",
    "InventionRegistry",
    "InventionError",
    "InventionStatus",
    "AUTO_RETIRE_AFTER_S",
]

DDR_ID = "C-04"
_FILE = "aether/agents/invention/registry.py"
CAPABILITY = "CAP_INVENTION_WRITE"

DEFAULT_STORE = Path(__file__).resolve().parent / "registry.jsonl"
AUTO_RETIRE_AFTER_S = 30 * 24 * 3600.0          # 30 days unused

InventionStatus = Literal["active", "deprecated", "retired"]


class InventionError(RuntimeError):
    """Registry operation refused."""


@dataclass(slots=True)
class Invention:
    inv_id: str
    source_ddr: str
    title: str
    description: str = ""
    capability_delta: dict[str, Any] = field(default_factory=dict)
    status: InventionStatus = "active"
    ts: float = field(default_factory=time.time)
    last_used_ts: float = 0.0
    reason: str = ""

    def to_json(self) -> str:
        return json.dumps(asdict(self), sort_keys=True, separators=(",", ":"))


class InventionRegistry:
    """Append-only registry of self-generated capabilities."""

    def __init__(
        self,
        lockbox: Lockbox,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
        now_fn: Callable[[], float] | None = None,
        auto_retire_after_s: float = AUTO_RETIRE_AFTER_S,
    ) -> None:
        self.lockbox = lockbox
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self.now_fn: Callable[[], float] = now_fn if now_fn is not None else time.time
        self.auto_retire_after_s = float(auto_retire_after_s)
        self._index: dict[str, Invention] = {}
        self._replay()

    def _replay(self) -> None:
        """Rebuild state by replaying the log in order; later records win."""
        if not self.store_path.is_file():
            return
        with self.store_path.open("r", encoding="utf-8") as fh:
            for lineno, raw in enumerate(fh, start=1):
                stripped = raw.strip()
                if not stripped:
                    continue
                try:
                    rec = json.loads(stripped)
                except json.JSONDecodeError as exc:
                    raise InventionError(
                        f"corrupt invention registry at line {lineno}"
                    ) from exc
                inv = Invention(
                    inv_id=rec["inv_id"], source_ddr=rec["source_ddr"],
                    title=rec["title"], description=rec.get("description", ""),
                    capability_delta=rec.get("capability_delta", {}),
                    status=rec.get("status", "active"),
                    ts=rec.get("ts", 0.0),
                    last_used_ts=rec.get("last_used_ts", 0.0),
                    reason=rec.get("reason", ""),
                )
                self._index[inv.inv_id] = inv

    def _append(self, inv: Invention) -> None:
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(inv.to_json() + "\n")

    # -- lifecycle -----------------------------------------------------------
    def add(
        self,
        source_ddr: str,
        title: str,
        description: str = "",
        capability_delta: dict[str, Any] | None = None,
    ) -> str:
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not title:
            raise InventionError("an invention needs a title")
        inv = Invention(
            inv_id=str(uuid.uuid4()),
            source_ddr=source_ddr,
            title=title,
            description=description,
            capability_delta=dict(capability_delta or {}),
            last_used_ts=self.now_fn(),
        )
        self._index[inv.inv_id] = inv
        self._append(inv)
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="invention.add", gate_status="active",
            inv_id=inv.inv_id, title=title, source=source_ddr,
        )
        return inv.inv_id

    def get(self, inv_id: str) -> Invention | None:
        return self._index.get(inv_id)

    def list_active(self) -> list[Invention]:
        return sorted(
            (i for i in self._index.values() if i.status == "active"),
            key=lambda i: i.ts,
        )

    def list_all(self) -> list[Invention]:
        return sorted(self._index.values(), key=lambda i: i.ts)

    def _transition(self, inv_id: str, status: InventionStatus, reason: str) -> Invention:
        self.lockbox.require(DDR_ID, CAPABILITY)
        inv = self._index.get(inv_id)
        if inv is None:
            raise InventionError(f"unknown invention: {inv_id}")
        if not reason:
            raise InventionError(f"{status} requires a reason")
        if inv.status == "retired":
            raise InventionError(f"{inv_id} is retired and cannot change status")
        # Append a NEW record; never edit the old one.
        updated = Invention(
            inv_id=inv.inv_id, source_ddr=inv.source_ddr, title=inv.title,
            description=inv.description, capability_delta=inv.capability_delta,
            status=status, ts=inv.ts, last_used_ts=inv.last_used_ts, reason=reason,
        )
        self._index[inv_id] = updated
        self._append(updated)
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action=f"invention.{status}",
            gate_status=status, inv_id=inv_id, reason=reason,
        )
        return updated

    def deprecate(self, inv_id: str, reason: str) -> Invention:
        return self._transition(inv_id, "deprecated", reason)

    def retire(self, inv_id: str, reason: str) -> Invention:
        return self._transition(inv_id, "retired", reason)

    def mark_used(self, inv_id: str) -> Invention:
        inv = self._index.get(inv_id)
        if inv is None:
            raise InventionError(f"unknown invention: {inv_id}")
        inv.last_used_ts = self.now_fn()
        self._append(inv)
        return inv

    # -- auto-retirement -----------------------------------------------------
    def auto_retire(self) -> list[Invention]:
        """Retire anything unused past the window. Runs on a 24 h schedule."""
        now = self.now_fn()
        retired: list[Invention] = []
        for inv in list(self._index.values()):
            if inv.status == "retired":
                continue
            reference = inv.last_used_ts or inv.ts
            age = now - reference
            if age > self.auto_retire_after_s:
                retired.append(
                    self._transition(
                        inv.inv_id, "retired",
                        f"unused for {age / 86400.0:.0f} days (auto)",
                    )
                )
        return retired
