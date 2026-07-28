"""CONFIRM-1 gate (4) — the shared egress rate limiter (S2)."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.kernel.audit.audit_log import AuditLog
from aether.platform.ratelimit.shared_limiter import (
    DEFAULT_RATE_PER_S,
    RateLimitExceeded,
    SharedRateLimiter,
)


class _Clock:
    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        return self.now

    def advance(self, dt: float) -> None:
        self.now += dt


def _limiter(tmp_path: Path, clock: _Clock, rate: float = 10.0,
             capacity: float = 10.0) -> SharedRateLimiter:
    return SharedRateLimiter(
        rate_per_s=rate, capacity=capacity,
        audit=AuditLog(tmp_path / "audit_log.jsonl"), clock=clock,
    )


def test_exhausting_one_path_blocks_the_other(tmp_path: Path) -> None:
    """THE discriminator — CONFIRM-1 gate (4), no bypass.

    A per-path limiter is the natural implementation and the actual failure:
    two paths at 60/s each is 120/s, so the limit the operator configured is
    not the limit the system obeys — and neither path's own tests would show
    it. A burst that exhausts the budget on one path must block the other.
    """
    clock = _Clock()
    limiter = _limiter(tmp_path, clock, rate=10.0, capacity=10.0)

    for _ in range(10):
        limiter.acquire("ollama")           # spend the whole budget locally

    with pytest.raises(RateLimitExceeded) as excinfo:
        limiter.acquire("cloud")            # the cloud path must be blocked too
    assert excinfo.value.path == "cloud"


def test_both_paths_draw_on_one_counter(tmp_path: Path) -> None:
    """Interleaved spend must sum, not run two budgets side by side."""
    clock = _Clock()
    limiter = _limiter(tmp_path, clock, rate=10.0, capacity=10.0)
    for i in range(10):
        limiter.acquire("cloud" if i % 2 else "ollama")
    assert limiter.total_spent() == 10
    with pytest.raises(RateLimitExceeded):
        limiter.acquire("ollama")


def test_path_is_recorded_but_never_routes(tmp_path: Path) -> None:
    """A brand-new path name must not get a fresh budget — which is exactly
    what a dict-of-buckets implementation would give it."""
    clock = _Clock()
    limiter = _limiter(tmp_path, clock, rate=5.0, capacity=5.0)
    for _ in range(5):
        limiter.acquire("ollama")
    with pytest.raises(RateLimitExceeded):
        limiter.acquire("a-path-never-seen-before")
    assert limiter.spent == {"ollama": 5}


def test_budget_refills_over_time(tmp_path: Path) -> None:
    clock = _Clock()
    limiter = _limiter(tmp_path, clock, rate=10.0, capacity=10.0)
    for _ in range(10):
        limiter.acquire("ollama")
    with pytest.raises(RateLimitExceeded):
        limiter.acquire("ollama")

    clock.advance(0.5)                      # 5 tokens back at 10/s
    for _ in range(5):
        limiter.acquire("cloud")
    with pytest.raises(RateLimitExceeded):
        limiter.acquire("cloud")


def test_refill_is_capped_at_capacity(tmp_path: Path) -> None:
    """Idle time must not bank unlimited burst — that is the fixed-window bug
    arriving by another route."""
    clock = _Clock()
    limiter = _limiter(tmp_path, clock, rate=10.0, capacity=10.0)
    limiter.acquire("ollama")
    clock.advance(3600.0)                   # an hour idle
    assert limiter.tokens == 10.0
    for _ in range(10):
        limiter.acquire("ollama")
    with pytest.raises(RateLimitExceeded):
        limiter.acquire("ollama")


def test_retry_after_is_computed_not_guessed(tmp_path: Path) -> None:
    clock = _Clock()
    limiter = _limiter(tmp_path, clock, rate=10.0, capacity=10.0)
    for _ in range(10):
        limiter.acquire("ollama")
    with pytest.raises(RateLimitExceeded) as excinfo:
        limiter.acquire("cloud")
    # One token short at 10/s is 0.1s.
    assert excinfo.value.retry_after_s == pytest.approx(0.1, abs=1e-6)


def test_exhaustion_is_audited_with_the_path(tmp_path: Path) -> None:
    clock = _Clock()
    limiter = _limiter(tmp_path, clock, rate=1.0, capacity=1.0)
    limiter.acquire("ollama")
    with pytest.raises(RateLimitExceeded):
        limiter.acquire("cloud")
    blocked = [e for e in AuditLog(tmp_path / "audit_log.jsonl").read_all()
               if e["action"] == "ratelimit.exhausted"]
    assert len(blocked) == 1
    assert blocked[0]["extra"]["path"] == "cloud"


def test_try_acquire_does_not_raise(tmp_path: Path) -> None:
    clock = _Clock()
    limiter = _limiter(tmp_path, clock, rate=1.0, capacity=1.0)
    assert limiter.try_acquire("ollama") is True
    assert limiter.try_acquire("cloud") is False


def test_tokens_read_refills_first(tmp_path: Path) -> None:
    """A stale read would let a caller think it has no budget when it does."""
    clock = _Clock()
    limiter = _limiter(tmp_path, clock, rate=10.0, capacity=10.0)
    for _ in range(10):
        limiter.acquire("ollama")
    assert limiter.tokens == pytest.approx(0.0)
    clock.advance(0.2)
    assert limiter.tokens == pytest.approx(2.0)


def test_default_rate_is_the_s2_figure() -> None:
    """CONFIRM-1 names 60/s explicitly; a drifted default silently changes the
    contract the gate refers to."""
    assert DEFAULT_RATE_PER_S == 60.0


@pytest.mark.parametrize(("rate", "capacity"), [(0.0, 10.0), (-1.0, 10.0),
                                                (10.0, 0.0)])
def test_nonsense_configuration_refused(rate: float, capacity: float) -> None:
    with pytest.raises(ValueError):
        SharedRateLimiter(rate_per_s=rate, capacity=capacity)


def test_zero_cost_refused(tmp_path: Path) -> None:
    """A free call is an unmetered call."""
    clock = _Clock()
    with pytest.raises(ValueError, match="cost must be > 0"):
        _limiter(tmp_path, clock).acquire("ollama", cost=0.0)


def test_bridge_shares_the_limiter_end_to_end(tmp_path: Path) -> None:
    """The real wire: the DDR-792 bridge spending from the shared bucket, and
    a cloud-path spend blocking it."""
    from aether.ollama_bridge.transport import (
        OllamaBridge,
        Response,
        RetryPolicy,
    )

    class _Transport:
        def send(self, endpoint: str, model: str, prompt: str,
                 timeout_s: float) -> Response:
            return Response("ok", model, 1, 1)

    clock = _Clock()
    limiter = _limiter(tmp_path, clock, rate=2.0, capacity=2.0)
    bridge = OllamaBridge(
        transport=_Transport(), audit=AuditLog(tmp_path / "audit_log.jsonl"),
        retry=RetryPolicy(jitter=lambda hi: hi), sleep=lambda _s: None,
        rate_limiter=limiter,
    )

    bridge.generate("llama3", "one")
    limiter.acquire("cloud")                # a cloud call spends the last token
    with pytest.raises(RateLimitExceeded):
        bridge.generate("llama3", "two")    # local is now blocked by cloud spend
    assert limiter.spent == {"ollama": 1, "cloud": 1}
