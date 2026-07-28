"""CONFIRM-1 gate (4) — the shared egress rate limiter (**S2**).

One token bucket, shared by every outbound path.

The rule CONFIRM-1 actually asks for is *no bypass*: cloud and local calls must
draw on the **same** counter. A per-path limiter is the natural implementation
and it is the failure: two paths at 60/s each is 120/s, so the limit the
operator set is not the limit the system obeys, and nothing in either path's
own tests would reveal it. So the counter is shared by construction —
:meth:`SharedRateLimiter.acquire` takes the path only to *record* which path
spent the token, never to select a bucket.

A token bucket rather than a fixed window: a fixed window lets a caller spend
the whole budget in the last millisecond of one window and the whole next budget
in the first millisecond of the following one, briefly running at twice the
configured rate. Bursts are bounded by capacity instead.

The clock is injected (**S9**) so the refill is testable without sleeping, and
so a gate cannot pass by accident of timing.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Any, Callable

from aether.kernel.audit.audit_log import AuditLog

__all__ = [
    "SharedRateLimiter",
    "RateLimitExceeded",
    "DEFAULT_RATE_PER_S",
    "DEFAULT_CAPACITY",
]

DDR_ID = "S2"
_FILE = "aether/platform/ratelimit/shared_limiter.py"

#: S2 — 60 syscalls/second, the figure CONFIRM-1 names.
DEFAULT_RATE_PER_S = 60.0
DEFAULT_CAPACITY = 60.0


class RateLimitExceeded(RuntimeError):
    """The shared budget is exhausted."""

    def __init__(self, path: str, retry_after_s: float) -> None:
        self.path = path
        self.retry_after_s = retry_after_s
        super().__init__(
            f"egress rate limit exhausted (path {path!r}); "
            f"retry in {retry_after_s:.3f}s"
        )


@dataclass(slots=True)
class SharedRateLimiter:
    """One bucket for all egress. The path is recorded, never routed on."""

    rate_per_s: float = DEFAULT_RATE_PER_S
    capacity: float = DEFAULT_CAPACITY
    audit: AuditLog | None = None
    clock: Callable[[], float] = time.monotonic
    alignment_tap: Callable[[str, dict[str, Any]], None] | None = None
    _tokens: float = field(default=-1.0, init=False)
    _last: float = field(default=-1.0, init=False)
    #: Per-path spend, for observability only. Never consulted by acquire().
    spent: dict[str, int] = field(default_factory=dict, init=False)

    def __post_init__(self) -> None:
        if self.rate_per_s <= 0:
            raise ValueError("rate_per_s must be > 0")
        if self.capacity <= 0:
            raise ValueError("capacity must be > 0")
        self._tokens = float(self.capacity)
        self._last = self.clock()

    def _emit(self, step: str, detail: dict[str, Any]) -> None:
        if self.alignment_tap is not None:
            self.alignment_tap(f"{DDR_ID}.{step}", detail)

    def _refill(self) -> None:
        now = self.clock()
        elapsed = max(0.0, now - self._last)
        self._last = now
        self._tokens = min(self.capacity, self._tokens + elapsed * self.rate_per_s)

    @property
    def tokens(self) -> float:
        """Current budget. Refills first, so a read is never stale."""
        self._refill()
        return self._tokens

    def acquire(self, path: str = "unknown", cost: float = 1.0) -> None:
        """Spend one unit of the SHARED budget, or raise.

        ``path`` is recorded and never used to pick a bucket — that is the
        difference between one limit and N limits wearing one name.
        """
        if cost <= 0:
            raise ValueError("cost must be > 0")
        self._refill()
        if self._tokens < cost:
            deficit = cost - self._tokens
            retry_after = deficit / self.rate_per_s
            if self.audit is not None:
                self.audit.append_action(
                    ddr=DDR_ID, file=_FILE, action="ratelimit.exhausted",
                    gate_status="blocked", path=path,
                    retry_after_s=round(retry_after, 6),
                )
            self._emit("exhausted", {"path": path})
            raise RateLimitExceeded(path, retry_after)
        self._tokens -= cost
        self.spent[path] = self.spent.get(path, 0) + 1
        self._emit("acquire", {"path": path, "remaining": self._tokens})

    def try_acquire(self, path: str = "unknown", cost: float = 1.0) -> bool:
        """Non-raising variant, for callers that can degrade instead of fail."""
        try:
            self.acquire(path, cost)
        except RateLimitExceeded:
            return False
        return True

    def total_spent(self) -> int:
        return sum(self.spent.values())
