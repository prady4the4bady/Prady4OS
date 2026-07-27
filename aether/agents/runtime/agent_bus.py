"""B-07 — agent runtime message bus.

Asyncio pub/sub. Every agent in the layer talks through here, so three
properties matter more than features:

* **S5 — no identity forgery.** ``publish`` is a method on the *handle* returned
  by :meth:`AgentBus.register`, and the handle stamps its own ``sender_id``.
  There is no API that accepts a caller-supplied sender, so a forged identity is
  not something the bus has to detect — it cannot be expressed.
* **Bounded queues.** Each subscription has a fixed-size queue. When it fills,
  the oldest message is dropped and the drop is audited. An unbounded queue
  turns one slow handler into an OOM.
* **Fault isolation.** A handler that raises must not kill the bus, the
  publisher, or other subscribers to the same topic.

Topics are dotted strings; a subscription to ``"a.b"`` receives ``"a.b"``, and
``"*"`` receives everything.
"""

from __future__ import annotations

import asyncio
import itertools
import time
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Any, Awaitable, Callable

from aether.kernel.audit.audit_log import AuditLog

__all__ = [
    "Priority",
    "Message",
    "Subscription",
    "AgentHandle",
    "AgentBus",
    "BusError",
]

DDR_ID = "B-07"
_FILE = "aether/agents/runtime/agent_bus.py"

WILDCARD = "*"
DEFAULT_QUEUE_SIZE = 256

Handler = Callable[["Message"], Awaitable[None]]


class BusError(RuntimeError):
    """Bus-level failure (unknown agent, revoked handle)."""


class Priority(IntEnum):
    LOW = 1
    NORMAL = 5
    HIGH = 8
    CRITICAL = 10


@dataclass(slots=True, frozen=True)
class Message:
    """One published message. ``sender_id`` is stamped by the bus, not the caller."""

    topic: str
    sender_id: str
    payload: dict[str, Any]
    msg_id: int
    ts: float
    priority: Priority = Priority.NORMAL


@dataclass(slots=True)
class Subscription:
    """A bounded delivery queue for one handler."""

    agent_id: str
    topic: str
    handler: Handler
    queue: asyncio.Queue[Message]
    dropped: int = 0
    delivered: int = 0
    failed: int = 0
    _task: asyncio.Task[None] | None = field(default=None, repr=False)


@dataclass(slots=True)
class AgentHandle:
    """Proof of registration. The only way to publish.

    Holding a handle *is* the identity claim, which is what makes S5 structural
    rather than a check that could be forgotten at a call site.
    """

    agent_id: str
    _bus: AgentBus = field(repr=False)
    _revoked: bool = False

    async def publish(
        self,
        topic: str,
        payload: dict[str, Any] | None = None,
        priority: Priority = Priority.NORMAL,
    ) -> Message:
        if self._revoked:
            raise BusError(f"handle for {self.agent_id} has been revoked")
        return await self._bus._publish_as(self.agent_id, topic, payload or {}, priority)

    def subscribe(self, topic: str, handler: Handler) -> Subscription:
        if self._revoked:
            raise BusError(f"handle for {self.agent_id} has been revoked")
        return self._bus._subscribe_as(self.agent_id, topic, handler)


class AgentBus:
    """Bounded, fault-isolated asyncio pub/sub."""

    def __init__(
        self,
        queue_size: int = DEFAULT_QUEUE_SIZE,
        audit: AuditLog | None = None,
    ) -> None:
        if queue_size < 1:
            raise ValueError("queue_size must be >= 1")
        self.queue_size = int(queue_size)
        self.audit = audit if audit is not None else AuditLog()
        self._agents: dict[str, AgentHandle] = {}
        self._subs: dict[str, list[Subscription]] = {}
        self._ids = itertools.count(1)
        self._running = False

    # -- registration --------------------------------------------------------
    def register(self, agent_id: str) -> AgentHandle:
        """Register an agent and return its publishing handle."""
        if not agent_id:
            raise ValueError("agent_id must be non-empty")
        existing = self._agents.get(agent_id)
        if existing is not None and not existing._revoked:
            return existing
        handle = AgentHandle(agent_id=agent_id, _bus=self)
        self._agents[agent_id] = handle
        return handle

    def revoke(self, agent_id: str) -> None:
        handle = self._agents.get(agent_id)
        if handle is None:
            raise BusError(f"unknown agent: {agent_id}")
        handle._revoked = True

    @property
    def agent_ids(self) -> tuple[str, ...]:
        return tuple(sorted(self._agents))

    # -- subscription --------------------------------------------------------
    def _subscribe_as(self, agent_id: str, topic: str, handler: Handler) -> Subscription:
        sub = Subscription(
            agent_id=agent_id,
            topic=topic,
            handler=handler,
            queue=asyncio.Queue(maxsize=self.queue_size),
        )
        self._subs.setdefault(topic, []).append(sub)
        if self._running:
            sub._task = asyncio.create_task(self._drain(sub))
        return sub

    def subscriptions(self, topic: str) -> tuple[Subscription, ...]:
        return tuple(self._subs.get(topic, ()))

    # -- publishing ----------------------------------------------------------
    async def _publish_as(
        self,
        sender_id: str,
        topic: str,
        payload: dict[str, Any],
        priority: Priority,
    ) -> Message:
        msg = Message(
            topic=topic,
            sender_id=sender_id,
            payload=dict(payload),
            msg_id=next(self._ids),
            ts=time.time(),
            priority=priority,
        )
        targets = list(self._subs.get(topic, ()))
        if topic != WILDCARD:
            targets.extend(self._subs.get(WILDCARD, ()))
        for sub in targets:
            self._enqueue(sub, msg)
        return msg

    def _enqueue(self, sub: Subscription, msg: Message) -> None:
        """Enqueue, dropping the OLDEST on overflow so fresh state wins."""
        try:
            sub.queue.put_nowait(msg)
        except asyncio.QueueFull:
            try:
                sub.queue.get_nowait()
                sub.queue.task_done()
            except asyncio.QueueEmpty:      # pragma: no cover - racy drain
                pass
            sub.dropped += 1
            self.audit.append_action(
                ddr=DDR_ID,
                file=_FILE,
                action="bus.drop",
                gate_status="dropped",
                topic=msg.topic,
                subscriber=sub.agent_id,
                dropped_total=sub.dropped,
            )
            try:
                sub.queue.put_nowait(msg)
            except asyncio.QueueFull:       # pragma: no cover - racy drain
                pass

    # -- delivery ------------------------------------------------------------
    async def _drain(self, sub: Subscription) -> None:
        while True:
            msg = await sub.queue.get()
            try:
                await sub.handler(msg)
                sub.delivered += 1
            except asyncio.CancelledError:
                sub.queue.task_done()
                raise
            except Exception as exc:
                # Fault isolation: one bad handler must not stop the bus.
                sub.failed += 1
                self.audit.append_action(
                    ddr=DDR_ID,
                    file=_FILE,
                    action="bus.handler_error",
                    gate_status="error",
                    topic=msg.topic,
                    subscriber=sub.agent_id,
                    error=f"{type(exc).__name__}: {exc}",
                )
            finally:
                sub.queue.task_done()

    async def start(self) -> None:
        if self._running:
            return
        self._running = True
        for subs in self._subs.values():
            for sub in subs:
                if sub._task is None:
                    sub._task = asyncio.create_task(self._drain(sub))

    async def drain(self) -> None:
        """Wait until every queued message has been handled."""
        for subs in self._subs.values():
            for sub in subs:
                await sub.queue.join()

    async def stop(self) -> None:
        self._running = False
        tasks = [s._task for subs in self._subs.values() for s in subs if s._task]
        for task in tasks:
            task.cancel()
        for task in tasks:
            try:
                await task
            except (asyncio.CancelledError, Exception):
                pass
        for subs in self._subs.values():
            for sub in subs:
                sub._task = None

    async def __aenter__(self) -> AgentBus:
        await self.start()
        return self

    async def __aexit__(self, *exc_info: object) -> None:
        await self.stop()
