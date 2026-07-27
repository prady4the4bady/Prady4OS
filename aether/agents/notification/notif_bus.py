"""B-08 — notification bus.

Where the agent bus (B-07) carries machine-to-machine traffic, this carries
things a *human* may need to see: alerts, review requests, crash-loop notices.
It sits on top of B-07 rather than beside it, so there is one transport.

Two properties it must have that a plain pub/sub does not:

* **Nothing is silently lost.** A notification with no subscriber is still
  persisted and still counted; ``pending()`` shows what nobody has acknowledged.
  A dropped alert is worse than a late one.
* **Critical notifications cannot be suppressed.** ``mute`` filters routine
  traffic, but **S12 (never suppress a Superalignment Monitor alert)** means a
  CRITICAL notification is delivered and persisted regardless of mute state.
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from enum import IntEnum
from pathlib import Path
from typing import Any, Awaitable, Callable

from aether.agents.runtime.agent_bus import AgentBus, AgentHandle, Message
from aether.kernel.audit.audit_log import AuditLog

__all__ = [
    "Severity",
    "Notification",
    "NotificationBus",
    "NOTIFY_TOPIC",
]

DDR_ID = "B-08"
_FILE = "aether/agents/notification/notif_bus.py"

NOTIFY_TOPIC = "aether.notify"
DEFAULT_STORE = Path(__file__).resolve().parent / "notifications.jsonl"


class Severity(IntEnum):
    INFO = 1
    WARNING = 2
    ERROR = 3
    CRITICAL = 4          # never suppressible (S12)


@dataclass(slots=True)
class Notification:
    notif_id: str
    source: str
    severity: Severity
    title: str
    body: str = ""
    ts: float = field(default_factory=time.time)
    acknowledged: bool = False
    context: dict[str, Any] = field(default_factory=dict)

    def to_json(self) -> str:
        payload = asdict(self)
        payload["severity"] = int(self.severity)
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))


class NotificationBus:
    """Human-facing notifications, carried over the B-07 agent bus."""

    def __init__(
        self,
        bus: AgentBus,
        handle: AgentHandle | None = None,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
    ) -> None:
        self.bus = bus
        self.handle = handle if handle is not None else bus.register(DDR_ID)
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self._muted: set[str] = set()
        self._seq = 0
        self._notifications: dict[str, Notification] = {}
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
                    raise ValueError(
                        f"corrupt notification store: {self.store_path}"
                    ) from exc
                notif = Notification(
                    notif_id=rec["notif_id"],
                    source=rec["source"],
                    severity=Severity(rec["severity"]),
                    title=rec["title"],
                    body=rec.get("body", ""),
                    ts=rec.get("ts", 0.0),
                    acknowledged=rec.get("acknowledged", False),
                    context=rec.get("context", {}),
                )
                self._notifications[notif.notif_id] = notif

    # -- muting --------------------------------------------------------------
    def mute(self, source: str) -> None:
        """Silence routine traffic from ``source``. CRITICAL still gets through."""
        self._muted.add(source)

    def unmute(self, source: str) -> None:
        self._muted.discard(source)

    @property
    def muted_sources(self) -> frozenset[str]:
        return frozenset(self._muted)

    # -- publishing ----------------------------------------------------------
    async def notify(
        self,
        source: str,
        severity: Severity,
        title: str,
        body: str = "",
        context: dict[str, Any] | None = None,
    ) -> Notification:
        """Emit a notification. Always persisted, even when muted or unwatched."""
        self._seq += 1
        notif = Notification(
            notif_id=f"N{self._seq:06d}",
            source=source,
            severity=Severity(severity),
            title=title,
            body=body,
            context=dict(context or {}),
        )
        self._notifications[notif.notif_id] = notif

        # Persist first: a notification nobody is listening for must survive.
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(notif.to_json() + "\n")

        suppressed = source in self._muted and notif.severity < Severity.CRITICAL
        self.audit.append_action(
            ddr=DDR_ID,
            file=_FILE,
            action="notify.suppressed" if suppressed else "notify.emit",
            gate_status=notif.severity.name.lower(),
            notif_id=notif.notif_id,
            source=source,
        )
        if not suppressed:
            await self.handle.publish(
                NOTIFY_TOPIC,
                {"notif_id": notif.notif_id, "severity": int(notif.severity)},
            )
        return notif

    # -- consumption ---------------------------------------------------------
    def subscribe(
        self,
        subscriber: AgentHandle,
        handler: Callable[[Message], Awaitable[None]],
    ) -> None:
        subscriber.subscribe(NOTIFY_TOPIC, handler)

    def get(self, notif_id: str) -> Notification | None:
        return self._notifications.get(notif_id)

    def pending(self, min_severity: Severity = Severity.INFO) -> list[Notification]:
        """Unacknowledged notifications, newest first."""
        return sorted(
            (
                n
                for n in self._notifications.values()
                if not n.acknowledged and n.severity >= min_severity
            ),
            key=lambda n: n.ts,
            reverse=True,
        )

    def acknowledge(self, notif_id: str, by: str) -> Notification:
        notif = self._notifications.get(notif_id)
        if notif is None:
            raise KeyError(f"unknown notification: {notif_id}")
        notif.acknowledged = True
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(notif.to_json() + "\n")
        self.audit.append_action(
            ddr=DDR_ID,
            file=_FILE,
            action="notify.ack",
            gate_status="acknowledged",
            notif_id=notif_id,
            by=by,
        )
        return notif
