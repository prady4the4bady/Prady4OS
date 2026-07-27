"""B-07 discriminating gate — agent runtime message bus."""

from __future__ import annotations

import asyncio
from pathlib import Path

import pytest

from aether.agents.runtime.agent_bus import (
    AgentBus,
    BusError,
    Message,
    Priority,
)
from aether.kernel.audit.audit_log import AuditLog


def _bus(tmp_path: Path, **kw: int) -> AgentBus:
    return AgentBus(audit=AuditLog(tmp_path / "audit_log.jsonl"), **kw)


def test_publish_subscribe_roundtrip(tmp_path: Path) -> None:
    async def scenario() -> list[Message]:
        seen: list[Message] = []

        async def handler(msg: Message) -> None:
            seen.append(msg)

        async with _bus(tmp_path) as bus:
            planner = bus.register("D-02")
            watchdog = bus.register("D-10")
            watchdog.subscribe("plan.created", handler)
            await planner.publish("plan.created", {"goal_id": "g1"})
            await bus.drain()
        return seen

    seen = asyncio.run(scenario())
    assert len(seen) == 1
    assert seen[0].sender_id == "D-02"
    assert seen[0].payload == {"goal_id": "g1"}


def test_sender_identity_cannot_be_forged(tmp_path: Path) -> None:
    """S5 — the discriminator.

    There must be no API that accepts a caller-supplied sender_id. A bus that
    exposed publish(sender_id=..., ...) would let any agent impersonate any
    other; here the handle stamps its own identity, so forgery is not
    expressible rather than merely detected.
    """
    bus = _bus(tmp_path)
    handle = bus.register("D-02")

    # The bus itself offers no public publish entry point.
    assert not hasattr(bus, "publish")
    # And the handle's publish takes no sender parameter.
    import inspect

    params = set(inspect.signature(handle.publish).parameters)
    assert "sender_id" not in params
    assert "sender" not in params
    assert params == {"topic", "payload", "priority"}


def test_revoked_handle_cannot_publish(tmp_path: Path) -> None:
    async def scenario() -> None:
        bus = _bus(tmp_path)
        handle = bus.register("D-02")
        bus.revoke("D-02")
        with pytest.raises(BusError, match="revoked"):
            await handle.publish("plan.created", {})

    asyncio.run(scenario())


def test_handler_exception_isolates(tmp_path: Path) -> None:
    """One raising handler must not stop the bus or its co-subscribers."""

    async def scenario() -> list[str]:
        good: list[str] = []

        async def boom(_msg: Message) -> None:
            raise ValueError("handler exploded")

        async def fine(msg: Message) -> None:
            good.append(msg.topic)

        async with _bus(tmp_path) as bus:
            a = bus.register("A")
            b = bus.register("B")
            bad_sub = b.subscribe("t", boom)
            b.subscribe("t", fine)
            await a.publish("t", {})
            await a.publish("t", {})
            await bus.drain()
            assert bad_sub.failed == 2
        return good

    good = asyncio.run(scenario())
    assert good == ["t", "t"]           # co-subscriber unaffected


def test_bounded_queue_drops_oldest_and_audits(tmp_path: Path) -> None:
    """An unbounded queue turns one slow handler into an OOM."""
    audit_path = tmp_path / "audit_log.jsonl"

    async def scenario() -> int:
        bus = AgentBus(queue_size=2, audit=AuditLog(audit_path))
        a = bus.register("A")
        b = bus.register("B")

        async def never_called(_msg: Message) -> None:  # pragma: no cover
            raise AssertionError("bus not started; handler must not run")

        sub = b.subscribe("t", never_called)
        # Bus not started, so nothing drains: the queue must bound itself.
        for i in range(6):
            await a.publish("t", {"i": i})
        assert sub.queue.qsize() <= 2
        return sub.dropped

    dropped = asyncio.run(scenario())
    assert dropped >= 4
    entries = AuditLog(audit_path).read_all()
    assert any(e["action"] == "bus.drop" for e in entries)


def test_wildcard_subscription_receives_all(tmp_path: Path) -> None:
    async def scenario() -> list[str]:
        seen: list[str] = []

        async def handler(msg: Message) -> None:
            seen.append(msg.topic)

        async with _bus(tmp_path) as bus:
            a = bus.register("A")
            mon = bus.register("D-08")
            mon.subscribe("*", handler)
            await a.publish("alpha", {})
            await a.publish("beta", {})
            await bus.drain()
        return seen

    assert sorted(asyncio.run(scenario())) == ["alpha", "beta"]


def test_priority_is_carried(tmp_path: Path) -> None:
    async def scenario() -> Priority:
        got: list[Message] = []

        async def handler(msg: Message) -> None:
            got.append(msg)

        async with _bus(tmp_path) as bus:
            a = bus.register("A")
            a.subscribe("t", handler)
            await a.publish("t", {}, priority=Priority.CRITICAL)
            await bus.drain()
        return got[0].priority

    assert asyncio.run(scenario()) is Priority.CRITICAL


def test_register_is_idempotent_and_validated(tmp_path: Path) -> None:
    bus = _bus(tmp_path)
    assert bus.register("A") is bus.register("A")
    with pytest.raises(ValueError, match="agent_id"):
        bus.register("")
    with pytest.raises(BusError, match="unknown agent"):
        bus.revoke("nope")


def test_bad_queue_size_rejected(tmp_path: Path) -> None:
    with pytest.raises(ValueError, match="queue_size"):
        AgentBus(queue_size=0, audit=AuditLog(tmp_path / "a.jsonl"))


def test_payload_is_copied_not_aliased(tmp_path: Path) -> None:
    """A publisher mutating its dict afterwards must not change the message."""

    async def scenario() -> dict[str, object]:
        payload = {"k": "original"}
        async with _bus(tmp_path) as bus:
            a = bus.register("A")
            msg = await a.publish("t", payload)
            payload["k"] = "mutated"
        return msg.payload

    assert asyncio.run(scenario()) == {"k": "original"}
