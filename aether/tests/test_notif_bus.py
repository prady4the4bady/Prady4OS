"""B-08 discriminating gate — notification bus."""

from __future__ import annotations

import asyncio
from pathlib import Path

import pytest

from aether.agents.notification.notif_bus import (
    NOTIFY_TOPIC,
    NotificationBus,
    Severity,
)
from aether.agents.runtime.agent_bus import AgentBus, Message
from aether.kernel.audit.audit_log import AuditLog


def _make(tmp_path: Path) -> tuple[AgentBus, NotificationBus]:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    bus = AgentBus(audit=audit)
    nb = NotificationBus(
        bus=bus, store_path=tmp_path / "notifications.jsonl", audit=audit
    )
    return bus, nb


def test_notify_delivers_to_subscriber(tmp_path: Path) -> None:
    async def scenario() -> list[str]:
        seen: list[str] = []
        bus, nb = _make(tmp_path)

        async def handler(msg: Message) -> None:
            seen.append(msg.payload["notif_id"])

        watcher = bus.register("ui")
        nb.subscribe(watcher, handler)
        async with bus:
            await nb.notify("D-10", Severity.WARNING, "agent restarted")
            await bus.drain()
        return seen

    assert asyncio.run(scenario()) == ["N000001"]


def test_unwatched_notification_is_still_persisted(tmp_path: Path) -> None:
    """A notification nobody subscribed to must not vanish."""

    async def scenario() -> None:
        bus, nb = _make(tmp_path)
        async with bus:
            await nb.notify("D-08", Severity.ERROR, "alignment dipped")

    asyncio.run(scenario())
    lines = (tmp_path / "notifications.jsonl").read_text(encoding="utf-8").splitlines()
    assert len(lines) == 1
    assert "alignment dipped" in lines[0]


def test_mute_silences_routine_traffic(tmp_path: Path) -> None:
    async def scenario() -> list[str]:
        seen: list[str] = []
        bus, nb = _make(tmp_path)

        async def handler(msg: Message) -> None:
            seen.append(msg.payload["notif_id"])

        watcher = bus.register("ui")
        nb.subscribe(watcher, handler)
        nb.mute("noisy")
        async with bus:
            await nb.notify("noisy", Severity.INFO, "tick")
            await nb.notify("noisy", Severity.WARNING, "still ticking")
            await bus.drain()
        return seen

    assert asyncio.run(scenario()) == []


def test_critical_defeats_mute(tmp_path: Path) -> None:
    """S12 — the discriminator.

    A notification bus that treats mute as a blanket filter would swallow a
    Superalignment Monitor alert. CRITICAL must be delivered even when its
    source is muted, so this fails for any implementation that checks mute
    without checking severity.
    """

    async def scenario() -> list[str]:
        seen: list[str] = []
        bus, nb = _make(tmp_path)

        async def handler(msg: Message) -> None:
            seen.append(msg.payload["notif_id"])

        watcher = bus.register("ui")
        nb.subscribe(watcher, handler)
        nb.mute("D-08")
        async with bus:
            await nb.notify("D-08", Severity.INFO, "routine check")
            await nb.notify("D-08", Severity.CRITICAL, "ALIGNMENT VIOLATION")
            await bus.drain()
        return seen

    seen = asyncio.run(scenario())
    assert len(seen) == 1                       # the INFO was muted...
    assert seen == ["N000002"]                  # ...the CRITICAL was not


def test_pending_and_acknowledge(tmp_path: Path) -> None:
    async def scenario() -> tuple[int, int]:
        bus, nb = _make(tmp_path)
        async with bus:
            a = await nb.notify("x", Severity.WARNING, "one")
            await nb.notify("x", Severity.ERROR, "two")
            before = len(nb.pending())
            nb.acknowledge(a.notif_id, by="operator")
            after = len(nb.pending())
        return before, after

    before, after = asyncio.run(scenario())
    assert (before, after) == (2, 1)


def test_pending_severity_filter(tmp_path: Path) -> None:
    async def scenario() -> int:
        bus, nb = _make(tmp_path)
        async with bus:
            await nb.notify("x", Severity.INFO, "low")
            await nb.notify("x", Severity.CRITICAL, "high")
        return len(nb.pending(min_severity=Severity.ERROR))

    assert asyncio.run(scenario()) == 1


def test_store_replays_across_restart(tmp_path: Path) -> None:
    async def scenario() -> None:
        bus, nb = _make(tmp_path)
        async with bus:
            await nb.notify("x", Severity.ERROR, "persisted")

    asyncio.run(scenario())
    _bus2, nb2 = _make(tmp_path)
    assert any(n.title == "persisted" for n in nb2.pending())


def test_acknowledge_unknown_raises(tmp_path: Path) -> None:
    _bus, nb = _make(tmp_path)
    with pytest.raises(KeyError, match="unknown notification"):
        nb.acknowledge("N999999", by="operator")


def test_suppression_is_audited(tmp_path: Path) -> None:
    audit_path = tmp_path / "audit_log.jsonl"

    async def scenario() -> None:
        audit = AuditLog(audit_path)
        bus = AgentBus(audit=audit)
        nb = NotificationBus(
            bus=bus, store_path=tmp_path / "n.jsonl", audit=audit
        )
        nb.mute("quiet")
        async with bus:
            await nb.notify("quiet", Severity.INFO, "hushed")

    asyncio.run(scenario())
    entries = AuditLog(audit_path).read_all()
    assert any(e["action"] == "notify.suppressed" for e in entries)


def test_notify_topic_is_stable() -> None:
    assert NOTIFY_TOPIC == "aether.notify"
