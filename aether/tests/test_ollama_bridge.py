"""DDR-792 gate — Ollama transport bridge.

S9: the transport is a stub in every test. No test here may require a live
Ollama endpoint — a live call in CI is a CI failure waiting to happen.
"""

from __future__ import annotations

import hashlib
from pathlib import Path

import pytest

from aether.kernel.audit.audit_log import AuditLog
from aether.ollama_bridge.transport import (
    MAX_ATTEMPTS,
    BridgeError,
    DeadlineExceeded,
    OllamaBridge,
    PrivacyModeViolation,
    Response,
    RetryPolicy,
    SemanticError,
    StreamInterrupted,
    TransportError,
)


class _Clock:
    """Injected monotonic clock — the deadline must be testable without waiting."""

    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        return self.now

    def advance(self, dt: float) -> None:
        self.now += dt


class _Transport:
    """Scripted wire. Each entry is either a Response or an exception to raise."""

    def __init__(self, *script: object, clock: _Clock | None = None,
                 cost_s: float = 0.0) -> None:
        self.script = list(script)
        self.calls: list[tuple[str, str, float]] = []
        self.clock = clock
        self.cost_s = cost_s

    def send(self, endpoint: str, model: str, prompt: str,
             timeout_s: float) -> Response:
        self.calls.append((model, prompt, timeout_s))
        if self.clock is not None and self.cost_s:
            # A real transport HONOURS timeout_s — it is the mechanism by which
            # the bridge's shrinking budget actually bounds wall-clock. Modelling
            # a transport that ignores it would test something no transport does.
            spent = min(self.cost_s, timeout_s)
            self.clock.advance(spent)
            if spent < self.cost_s:
                raise TransportError("connect timed out", "timeout")
        item = self.script.pop(0) if self.script else Response("ok", model, 3, 4)
        if isinstance(item, Exception):
            raise item
        assert isinstance(item, Response)
        return item


def _bridge(tmp_path: Path, transport: _Transport, clock: _Clock | None = None,
            **kw: object) -> OllamaBridge:
    clock = clock or _Clock()
    slept: list[float] = []
    bridge = OllamaBridge(
        transport=transport,
        audit=AuditLog(tmp_path / "audit_log.jsonl"),
        clock=clock,
        sleep=lambda s: (slept.append(s), clock.advance(s))[0],
        retry=RetryPolicy(jitter=lambda hi: hi),   # deterministic for gating
        **kw,  # type: ignore[arg-type]
    )
    bridge._slept = slept          # type: ignore[attr-defined]
    bridge._clock = clock          # type: ignore[attr-defined]
    return bridge


def _entries(tmp_path: Path) -> list[dict]:
    return AuditLog(tmp_path / "audit_log.jsonl").read_all()


# -- retry: transport yes, semantics no ---------------------------------------
def test_transport_errors_are_retried(tmp_path: Path) -> None:
    t = _Transport(TransportError("refused", "connect"),
                   TransportError("refused", "connect"),
                   Response("recovered", "llama3", 2, 3))
    bridge = _bridge(tmp_path, t)
    assert bridge.generate("llama3", "hi").text == "recovered"
    assert len(t.calls) == 3


@pytest.mark.parametrize("status", [400, 404, 422])
def test_semantic_errors_are_never_retried(tmp_path: Path, status: int) -> None:
    """THE discriminator (half one).

    A 4xx means the request is wrong; re-sending sends the same wrong request.
    A naive 'retry on any exception' passes the transport test above and fails
    here — and in production it triples the load caused by a bad request.
    """
    t = _Transport(SemanticError(f"bad request {status}", status))
    with pytest.raises(SemanticError):
        _bridge(tmp_path, t).generate("llama3", "hi")
    assert len(t.calls) == 1, "a semantic error was retried"


def test_read_timeout_after_first_byte_is_not_retried(tmp_path: Path) -> None:
    """THE discriminator (half two).

    The model is already generating. Retrying doubles the cost and may double
    any side effect. This is the case that separates a real retry policy from
    'retry on timeout'.
    """
    t = _Transport(StreamInterrupted("stalled mid-stream", "read_timeout"))
    with pytest.raises(StreamInterrupted):
        _bridge(tmp_path, t).generate("llama3", "hi")
    assert len(t.calls) == 1, "a mid-stream stall was retried"


def test_attempts_are_capped(tmp_path: Path) -> None:
    t = _Transport(*[TransportError("refused", "connect") for _ in range(10)])
    with pytest.raises(TransportError):
        _bridge(tmp_path, t).generate("llama3", "hi")
    assert len(t.calls) == MAX_ATTEMPTS


# -- deadline ------------------------------------------------------------------
def test_deadline_covers_all_attempts_combined(tmp_path: Path) -> None:
    """THE discriminator (half three).

    30s is a budget for the whole call, not per attempt. A per-attempt timeout
    passes every other test here and turns a '30s timeout' into a 90s stall.
    """
    clock = _Clock()
    # Each attempt burns 20s: attempt 1 ends at t=20, and a second attempt
    # cannot fit inside a 30s budget.
    t = _Transport(*[TransportError("refused", "connect") for _ in range(5)],
                   clock=clock, cost_s=20.0)
    bridge = _bridge(tmp_path, t, clock=clock, deadline_s=30.0)
    with pytest.raises(DeadlineExceeded):
        bridge.generate("llama3", "hi")
    assert clock.now <= 30.0, f"the call ran past its deadline to {clock.now}s"
    assert len(t.calls) < MAX_ATTEMPTS


def test_backoff_that_would_exceed_the_deadline_fails_at_it(tmp_path: Path) -> None:
    """Sleeping past the budget is the same defect wearing a different hat."""
    clock = _Clock()
    t = _Transport(*[TransportError("refused", "connect") for _ in range(5)],
                   clock=clock, cost_s=14.9)
    bridge = _bridge(tmp_path, t, clock=clock, deadline_s=30.0)
    with pytest.raises(DeadlineExceeded):
        bridge.generate("llama3", "hi")
    assert clock.now <= 30.0


def test_timeout_passed_to_transport_never_exceeds_the_remaining_budget(
    tmp_path: Path,
) -> None:
    clock = _Clock()
    t = _Transport(Response("ok", "llama3", 1, 1), clock=clock)
    bridge = _bridge(tmp_path, t, clock=clock, deadline_s=3.0)
    bridge.generate("llama3", "hi")
    assert t.calls[0][2] <= 3.0


# -- privacy mode --------------------------------------------------------------
def test_privacy_mode_blocks_before_any_socket_is_opened(tmp_path: Path) -> None:
    """A check the caller performs is a check the next caller forgets — and a
    check performed after the response has already leaked the data."""
    t = _Transport(Response("should never happen", "llama3"))
    bridge = _bridge(tmp_path, t, privacy_fn=lambda: True)
    with pytest.raises(PrivacyModeViolation):
        bridge.generate("llama3", "hi")
    assert t.calls == [], "the transport was touched despite privacy mode"


def test_privacy_block_consumes_no_rate_limit_quota(tmp_path: Path) -> None:
    """A blocked call must not spend budget it never used."""
    class _Limiter:
        def __init__(self) -> None:
            self.taken: list[str] = []

        def acquire(self, path: str) -> None:
            self.taken.append(path)

    limiter = _Limiter()
    bridge = _bridge(tmp_path, _Transport(), privacy_fn=lambda: True,
                     rate_limiter=limiter)
    with pytest.raises(PrivacyModeViolation):
        bridge.generate("llama3", "hi")
    assert limiter.taken == []


def test_privacy_mode_is_read_at_call_time_not_construction(tmp_path: Path) -> None:
    """A flag captured at construction goes stale exactly when it matters."""
    state = {"private": False}
    t = _Transport(Response("ok", "llama3", 1, 1), Response("ok", "llama3", 1, 1))
    bridge = _bridge(tmp_path, t, privacy_fn=lambda: state["private"])
    bridge.generate("llama3", "hi")                 # allowed
    state["private"] = True
    with pytest.raises(PrivacyModeViolation):       # now blocked
        bridge.generate("llama3", "hi")


def test_privacy_block_is_audited(tmp_path: Path) -> None:
    bridge = _bridge(tmp_path, _Transport(), privacy_fn=lambda: True)
    with pytest.raises(PrivacyModeViolation):
        bridge.generate("llama3", "hi")
    blocked = [e for e in _entries(tmp_path) if e["action"] == "ollama.blocked"]
    assert blocked and blocked[0]["gate_status"] == "privacy"


# -- rate limiter --------------------------------------------------------------
def test_rate_limiter_is_consumed_on_every_call(tmp_path: Path) -> None:
    class _Limiter:
        def __init__(self) -> None:
            self.taken: list[str] = []

        def acquire(self, path: str) -> None:
            self.taken.append(path)

    limiter = _Limiter()
    t = _Transport(Response("a", "llama3"), Response("b", "llama3"))
    bridge = _bridge(tmp_path, t, rate_limiter=limiter)
    bridge.generate("llama3", "one")
    bridge.generate("llama3", "two")
    assert limiter.taken == ["ollama", "ollama"]


# -- audit ---------------------------------------------------------------------
def test_successful_call_audits_tokens_and_latency(tmp_path: Path) -> None:
    t = _Transport(Response("hello", "llama3", prompt_tokens=7,
                            completion_tokens=11))
    _bridge(tmp_path, t).generate("llama3", "hi")
    calls = [e for e in _entries(tmp_path) if e["action"] == "ollama.call"]
    assert len(calls) == 1
    extra = calls[0]["extra"]
    assert extra["token_count"] == 18
    assert extra["latency_ms"] >= 0.0
    assert extra["total_latency_ms"] >= 0.0
    assert extra["model"] == "llama3"


def test_prompt_is_hashed_never_logged(tmp_path: Path) -> None:
    """The audit log is append-only and widely read; putting prompt text in it
    makes every audit reader a reader of whatever the user typed."""
    secret = "the launch code is hunter2 correct-horse"
    t = _Transport(Response("ok", "llama3", 1, 1))
    _bridge(tmp_path, t).generate("llama3", secret)

    raw = (tmp_path / "audit_log.jsonl").read_text(encoding="utf-8")
    assert "hunter2" not in raw
    assert "correct-horse" not in raw
    expected = hashlib.sha256(secret.encode("utf-8")).hexdigest()
    assert expected in raw


def test_failure_after_all_attempts_is_audited(tmp_path: Path) -> None:
    t = _Transport(*[TransportError("refused", "connect") for _ in range(5)])
    with pytest.raises(TransportError):
        _bridge(tmp_path, t).generate("llama3", "hi")
    failed = [e for e in _entries(tmp_path) if e["action"] == "ollama.failed"]
    assert failed and failed[0]["extra"]["error_kind"] == "connect"
    retries = [e for e in _entries(tmp_path) if e["action"] == "ollama.retry"]
    assert len(retries) == MAX_ATTEMPTS


def test_semantic_error_is_audited_as_refused_not_retried(tmp_path: Path) -> None:
    t = _Transport(SemanticError("bad", 400))
    with pytest.raises(SemanticError):
        _bridge(tmp_path, t).generate("llama3", "hi")
    statuses = [e["gate_status"] for e in _entries(tmp_path)
                if e["action"] == "ollama.retry"]
    assert statuses == ["refused"]


def test_truncated_response_is_flagged_in_the_audit(tmp_path: Path) -> None:
    t = _Transport(Response("partial", "llama3", 1, 1, truncated=True))
    _bridge(tmp_path, t).generate("llama3", "hi")
    calls = [e for e in _entries(tmp_path) if e["action"] == "ollama.call"]
    assert calls[0]["extra"]["truncated"] is True


# -- backoff -------------------------------------------------------------------
def test_backoff_grows_and_is_jittered() -> None:
    """Without jitter, N agents failing together retry together and synchronise
    into a herd against one local server."""
    fixed = RetryPolicy(jitter=lambda hi: hi)
    assert fixed.backoff_for(1) < fixed.backoff_for(2) < fixed.backoff_for(3)

    seen = {RetryPolicy(jitter=lambda hi: hi * 0.5).backoff_for(2),
            RetryPolicy(jitter=lambda hi: hi).backoff_for(2)}
    assert len(seen) == 2, "jitter is not applied to the backoff ceiling"


# -- input validation ----------------------------------------------------------
@pytest.mark.parametrize(("model", "prompt", "match"), [
    ("", "hi", "needs a model"),
    ("llama3", "", "needs a prompt"),
])
def test_malformed_calls_refused(tmp_path: Path, model, prompt, match) -> None:
    t = _Transport()
    with pytest.raises(BridgeError, match=match):
        _bridge(tmp_path, t).generate(model, prompt)
    assert t.calls == []


def test_no_live_endpoint_is_required_anywhere_here() -> None:
    """S9 — this file must never need a real server.

    Pinned as an assertion rather than a convention: the transport protocol is
    what CI substitutes, and a bridge that reached for a default real transport
    would make the whole suite depend on a running Ollama.
    """
    import inspect

    from aether.ollama_bridge import transport as mod

    sig = inspect.signature(OllamaBridge.__init__)
    assert sig.parameters["transport"].default is inspect.Parameter.empty, (
        "transport has a default — CI could silently acquire a real one"
    )
    src = inspect.getsource(mod)
    for forbidden in ("import requests", "import httpx", "import urllib",
                      "import socket", "import http.client"):
        assert forbidden not in src, f"{forbidden} in the bridge module"
