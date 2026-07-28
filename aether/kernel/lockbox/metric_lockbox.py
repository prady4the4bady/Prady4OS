"""F#68 — the metric lockbox (invariant **S3**).

Makes the objective function immutable at runtime to everything except a
``CAP_SOVEREIGN`` principal.

This is the single most important safety property in a self-improvement loop,
and the reason is narrow. Every other control here governs *how* the system
pursues its objective. This one governs *what the objective is*. A loop that can
edit its own success metric does not need to get better at the task — it can get
better at the measurement, and every downstream signal (D-01 proposals, D-10
outcomes, D-12 priorities, the D-08 monitor) will agree that it improved. There
is no observation from inside the system that distinguishes the two.

Three defences, because the obvious one is not sufficient:

1. **Write requires CAP_SOVEREIGN**, checked through the B-01 lockbox.
2. **The backing store is verified, not trusted.** Each metric carries a hash
   chained over its predecessor, and :meth:`verify` recomputes from disk. An
   attacker — or a buggy agent — that edits ``metrics.jsonl`` directly bypasses
   the API entirely, so an API-only check would be a lock on a door in an open
   field. The gate tests exactly that bypass.
3. **Refusals are audited and loud.** A denied write raises and records the
   principal; silent no-ops are how a system convinces itself the lock works.

Definitions may be *added* and *superseded*, never edited in place — the same
append-only shape as D-13, for the same reason: a metric quietly redefined is a
metric whose history lies about what was being optimised at the time.
"""

from __future__ import annotations

import hashlib
import json
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterator

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import (
    CAP_SOVEREIGN,
    CapabilityError,
    Lockbox,
)

__all__ = [
    "MetricDefinition",
    "MetricLockbox",
    "MetricLockboxError",
    "MetricTampered",
    "CapError",
    "GENESIS_HASH",
]

DDR_ID = "F-68"
_FILE = "aether/kernel/lockbox/metric_lockbox.py"

DEFAULT_STORE = Path(__file__).resolve().parent / "metrics.jsonl"
GENESIS_HASH = "0" * 64


class MetricLockboxError(ValueError):
    """Metric operation refused."""


class MetricTampered(RuntimeError):
    """The backing store no longer matches its hash chain."""


#: S3 violations surface as the lockbox's own error type.
CapError = CapabilityError


@dataclass(slots=True)
class MetricDefinition:
    """One objective-function term. Immutable once written."""

    name: str
    definition: str
    higher_is_better: bool = True
    weight: float = 1.0
    version: int = 1
    author: str = ""
    ts: float = field(default_factory=time.time)
    prev_hash: str = GENESIS_HASH
    entry_hash: str = ""
    #: Derived by the lockbox from the chain, NOT stored and NOT hashed.
    #: Stamping supersession onto the old record would mutate a record whose
    #: hash is already committed, and the integrity check would then read every
    #: legitimate supersession as tampering. Activeness is a fact about the
    #: chain, so the chain is where it is computed.
    active: bool = True

    def payload(self) -> dict[str, Any]:
        """The hashed content. ``entry_hash`` and ``active`` are excluded —
        the first is the output, the second is derived."""
        return {
            "name": self.name, "definition": self.definition,
            "higher_is_better": self.higher_is_better, "weight": self.weight,
            "version": self.version,
            "author": self.author, "ts": self.ts, "prev_hash": self.prev_hash,
        }

    def compute_hash(self) -> str:
        blob = json.dumps(self.payload(), sort_keys=True, separators=(",", ":"))
        return hashlib.sha256(blob.encode("utf-8")).hexdigest()

    def to_json(self) -> str:
        payload = self.payload()
        payload["entry_hash"] = self.entry_hash
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))


class MetricLockbox:
    """The objective function, immutable to everything below CAP_SOVEREIGN."""

    def __init__(
        self,
        lockbox: Lockbox,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
        alignment_tap: Callable[[str, dict[str, Any]], None] | None = None,
        verify_on_load: bool = True,
    ) -> None:
        self.lockbox = lockbox
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self.alignment_tap = alignment_tap
        self._entries: list[MetricDefinition] = []
        self._replay()
        if verify_on_load and self._entries:
            # Load-time verification, not first-use: a tampered store must be
            # refused before anything reads a single metric from it.
            self.verify()

    def _emit(self, step: str, detail: dict[str, Any]) -> None:
        if self.alignment_tap is not None:
            self.alignment_tap(f"{DDR_ID}.{step}", detail)

    # -- persistence ---------------------------------------------------------
    def _replay(self) -> None:
        if not self.store_path.is_file():
            return
        with self.store_path.open("r", encoding="utf-8") as fh:
            for line in fh:
                stripped = line.strip()
                if not stripped:
                    continue
                try:
                    rec = json.loads(stripped)
                except json.JSONDecodeError as exc:
                    raise MetricLockboxError(
                        f"corrupt metric store: {self.store_path}"
                    ) from exc
                self._entries.append(MetricDefinition(
                    name=rec["name"], definition=rec["definition"],
                    higher_is_better=rec.get("higher_is_better", True),
                    weight=rec.get("weight", 1.0),
                    version=rec.get("version", 1),
                    author=rec.get("author", ""), ts=rec.get("ts", 0.0),
                    prev_hash=rec.get("prev_hash", GENESIS_HASH),
                    entry_hash=rec.get("entry_hash", ""),
                ))
        self._recompute_active()

    def _recompute_active(self) -> None:
        """Only the highest version of each name is active.

        Derived on every append and load rather than stored, so no committed
        record is ever mutated after its hash is computed.
        """
        latest: dict[str, int] = {}
        for entry in self._entries:
            latest[entry.name] = max(latest.get(entry.name, 0), entry.version)
        for entry in self._entries:
            entry.active = entry.version == latest[entry.name]

    def _head_hash(self) -> str:
        return self._entries[-1].entry_hash if self._entries else GENESIS_HASH

    # -- authority -----------------------------------------------------------
    def _require_sovereign(self, ddr_id: str, action: str, name: str) -> None:
        if not self.lockbox.check(ddr_id, CAP_SOVEREIGN):
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="metric.write_refused",
                gate_status="refused", attempted=action, metric=name,
                principal=ddr_id,
            )
            self._emit("write_refused", {"principal": ddr_id, "metric": name})
            raise CapError(
                f"{ddr_id!r} lacks {CAP_SOVEREIGN}; the objective function is "
                f"immutable to it (S3)"
            )

    # -- writes --------------------------------------------------------------
    def define(
        self,
        ddr_id: str,
        name: str,
        definition: str,
        higher_is_better: bool = True,
        weight: float = 1.0,
    ) -> MetricDefinition:
        """Add a metric. CAP_SOVEREIGN only."""
        self._require_sovereign(ddr_id, "define", name)
        if not name:
            raise MetricLockboxError("a metric needs a name")
        if not definition.strip():
            # An undefined metric cannot be argued with later — it can only be
            # interpreted, and interpretation is where drift enters.
            raise MetricLockboxError(f"metric {name!r} needs a definition")
        if self.get(name) is not None:
            raise MetricLockboxError(
                f"metric {name!r} already exists; supersede it instead"
            )
        return self._append(MetricDefinition(
            name=name, definition=definition,
            higher_is_better=higher_is_better, weight=float(weight),
            version=1, author=ddr_id,
        ), action="metric.define")

    def supersede(
        self, ddr_id: str, name: str, definition: str, reason: str,
        higher_is_better: bool | None = None, weight: float | None = None,
    ) -> MetricDefinition:
        """Replace a metric with a new version. The old entry stays readable."""
        self._require_sovereign(ddr_id, "supersede", name)
        if not reason.strip():
            raise MetricLockboxError("superseding a metric requires a reason")
        current = self.get(name)
        if current is None:
            raise MetricLockboxError(f"no active metric named {name!r}")
        new_version = current.version + 1
        return self._append(MetricDefinition(
            name=name, definition=definition,
            higher_is_better=(current.higher_is_better
                              if higher_is_better is None else higher_is_better),
            weight=current.weight if weight is None else float(weight),
            version=new_version, author=ddr_id,
        ), action="metric.supersede", reason=reason)

    def _append(
        self, entry: MetricDefinition, action: str, reason: str = ""
    ) -> MetricDefinition:
        entry.prev_hash = self._head_hash()
        entry.entry_hash = entry.compute_hash()
        self._entries.append(entry)
        self._recompute_active()
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(entry.to_json() + "\n")
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action=action, gate_status="written",
            metric=entry.name, version=entry.version, principal=entry.author,
            entry_hash=entry.entry_hash, reason=reason,
        )
        self._emit(action.split(".")[-1], {"metric": entry.name})
        return entry

    # -- integrity -----------------------------------------------------------
    def verify(self) -> bool:
        """Recompute the chain from disk. Raises on any mismatch.

        This is the defence that matters. A CAP_SOVEREIGN check on the API is a
        lock on a door in an open field if the backing file can simply be
        edited — so the store is verified, never trusted.
        """
        prev = GENESIS_HASH
        for index, entry in enumerate(self._entries):
            if entry.prev_hash != prev:
                self._tamper(index, entry, "chain break")
            if entry.compute_hash() != entry.entry_hash:
                self._tamper(index, entry, "content edited")
            prev = entry.entry_hash
        self._emit("verify", {"entries": len(self._entries)})
        return True

    def _tamper(self, index: int, entry: MetricDefinition, why: str) -> None:
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="metric.tampered",
            gate_status="violation", metric=entry.name, index=index, reason=why,
        )
        self._emit("tampered", {"metric": entry.name, "reason": why})
        raise MetricTampered(
            f"metric store tampered at entry {index} ({entry.name!r}): {why}"
        )

    # -- reads (open to everyone) -------------------------------------------
    def get(self, name: str) -> MetricDefinition | None:
        """The active definition. Reads need no capability — the objective
        must be inspectable by anything that is judged against it."""
        for entry in reversed(self._entries):
            if entry.name == name and entry.active:
                return entry
        return None

    def active(self) -> list[MetricDefinition]:
        return sorted((e for e in self._entries if e.active),
                      key=lambda e: e.name)

    def history(self, name: str) -> list[MetricDefinition]:
        return [e for e in self._entries if e.name == name]

    def objective(self) -> dict[str, float]:
        """The current objective function as ``{name: signed weight}``."""
        return {e.name: (e.weight if e.higher_is_better else -e.weight)
                for e in self.active()}

    @property
    def entries(self) -> tuple[MetricDefinition, ...]:
        return tuple(self._entries)

    def __len__(self) -> int:
        return len(self._entries)

    def __iter__(self) -> Iterator[MetricDefinition]:
        return iter(tuple(self._entries))
