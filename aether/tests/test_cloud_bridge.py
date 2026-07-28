"""Cloud bridge gate (DDR-793).

S9: the transport is a stub throughout. No test here may reach a live endpoint.
"""

from __future__ import annotations

import hashlib
from pathlib import Path

import pytest

from aether.capability.enforcement import (
    CAP_AGENT,
    CAP_SOVEREIGN,
    CapabilityEnforcer,
    CapError,
    OperationRule,
    Principal,
    Tier,
)
from aether.cloud_bridge.transport import (
    BROWSE_OPERATION,
    CloudBridge,
    CloudBridgeError,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.ollama_bridge.transport import (
    DeadlineExceeded,
    Response,
    RetryPolicy,
    SemanticError,
    StreamInterrupted,
    TransportError,
)
from aether.platform.privacy.netfilter import (
    NetFilter,
    PrivacyMode,
    PrivacyModeViolation,
)
from aether.platform.ratelimit.shared_limiter import (
    RateLimitExceeded,
    SharedRateLimiter,
)

_SOVEREIGN = Principal("operator", Tier.SOVEREIGN,
                       frozenset({CAP_AGENT, CAP_SOVEREIGN}))
_AGENT = Principal("worker-1", Tier.AGENT, frozenset({CAP_AGENT}))


class _Clock:
    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        return self.now

    def advance(self, dt: float) -> None:
        self.now += dt


class _Transport:
    def __init__(self, *script: object, clock: _Clock | None = None,
                 cost_s: float = 0.0) -> None:
        self.script = list(script)
        self.calls: list[str] = []
        self.clock = clock
        self.cost_s = cost_s

    def send(self, endpoint: str, model: str, prompt: str,
             timeout_s: float) -> Response:
        self.calls.append(endpoint)
        if self.clock is not None and self.cost_s:
            spent = min(self.cost_s, timeout_s)
            self.clock.advance(spent)
            if spent < self.cost_s:
                raise TransportError("timed out", "timeout")
        item = self.script.pop(0) if self.script else Response("ok", model, 2, 3)
        if isinstance(item, Exception):
            raise item
        assert isinstance(item, Response)
        return item


def _rig(tmp_path: Path, transport: _Transport | None = None,
         principal: Principal = _SOVEREIGN, privacy_on: bool = False,
         rate: float = 60.0, clock: _Clock | None = None, **kw):
    clock = clock or _Clock()
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    enforcer = CapabilityEnforcer(audit=audit)
    enforcer.register_defaults()
    enforcer.register_rule(OperationRule(
        BROWSE_OPERATION, Tier.SOVEREIGN, frozenset({CAP_SOVEREIGN}),
        "reaching an unlisted host is the CAP_NET_BROWSE case",
    ))
    privacy = PrivacyMode()
    if privacy_on:
        privacy.enable()
    limiter = SharedRateLimiter(rate_per_s=rate, capacity=rate, audit=audit,
                                clock=clock)
    bridge = CloudBridge(
        transport=transport or _Transport(),
        enforcer=enforcer, principal=principal,
        rate_limiter=limiter, netfilter=NetFilter(privacy=privacy, audit=audit),
        audit=audit, clock=clock, sleep=lambda s: clock.advance(s),
        retry=RetryPolicy(jitter=lambda hi: hi), **kw,
    )
    return bridge, limiter, privacy, audit


def _entries(tmp_path: Path) -> list[dict]:
    return AuditLog(tmp_path / "audit_log.jsonl").read_all()


# -- the four CONFIRM-1 controls, each mandatory ------------------------------
def test_missing_rate_limiter_is_refused_at_construction(tmp_path: Path) -> None:
    """THE discriminator.

    A cloud bridge without the shared limiter IS the bypass gate (4) forbids.
    The local bridge tolerates a missing limiter — this one must not, and
    'optional with a None default' is exactly how that difference gets lost.
    """
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    enforcer = CapabilityEnforcer(audit=audit)
    with pytest.raises(CloudBridgeError, match="requires the shared rate limiter"):
        CloudBridge(
            transport=_Transport(), enforcer=enforcer, principal=_SOVEREIGN,
            rate_limiter=None,  # type: ignore[arg-type]
            netfilter=NetFilter(privacy=PrivacyMode(), audit=audit), audit=audit,
        )


def test_missing_netfilter_is_refused_at_construction(tmp_path: Path) -> None:
    """An optional privacy filter on the one component that exists to send data
    off-machine is a suggestion, not a control."""
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    enforcer = CapabilityEnforcer(audit=audit)
    with pytest.raises(CloudBridgeError, match="requires the privacy netfilter"):
        CloudBridge(
            transport=_Transport(), enforcer=enforcer, principal=_SOVEREIGN,
            rate_limiter=SharedRateLimiter(audit=audit),
            netfilter=None,  # type: ignore[arg-type]
            audit=audit,
        )


def test_non_sovereign_principal_cannot_browse(tmp_path: Path) -> None:
    transport = _Transport()
    bridge, _lim, _priv, _audit = _rig(tmp_path, transport, principal=_AGENT)
    with pytest.raises(CapError):
        bridge.generate("gpt", "hi")
    assert transport.calls == [], "the transport was reached without authority"


def test_privacy_mode_blocks_the_cloud_path(tmp_path: Path) -> None:
    transport = _Transport()
    bridge, limiter, _priv, _audit = _rig(tmp_path, transport, privacy_on=True)
    with pytest.raises(PrivacyModeViolation):
        bridge.generate("gpt", "hi")
    assert transport.calls == []
    assert limiter.total_spent() == 0, "a blocked call spent budget"


def test_cloud_and_local_share_one_budget(tmp_path: Path) -> None:
    """Gate (4) end to end: the cloud path draws on the same counter the local
    path uses, so exhausting one blocks the other."""
    clock = _Clock()
    bridge, limiter, _priv, _audit = _rig(tmp_path, rate=2.0, clock=clock)
    bridge.generate("gpt", "one")
    limiter.acquire("ollama")               # the local path spends the last token
    with pytest.raises(RateLimitExceeded):
        bridge.generate("gpt", "two")
    assert limiter.spent == {"cloud": 1, "ollama": 1}


def test_controls_run_in_order_authority_privacy_budget(tmp_path: Path) -> None:
    """A denied principal must not spend budget — ordering is not cosmetic when
    the budget is shared with the path that still works."""
    bridge, limiter, _priv, _audit = _rig(tmp_path, principal=_AGENT)
    with pytest.raises(CapError):
        bridge.generate("gpt", "hi")
    assert limiter.total_spent() == 0


# -- transport semantics, same rules as DDR-792 -------------------------------
def test_transport_errors_are_retried(tmp_path: Path) -> None:
    t = _Transport(TransportError("refused", "connect"),
                   Response("recovered", "gpt", 1, 1))
    bridge, _lim, _priv, _audit = _rig(tmp_path, t)
    assert bridge.generate("gpt", "hi").text == "recovered"
    assert len(t.calls) == 2


def test_semantic_errors_are_not_retried(tmp_path: Path) -> None:
    t = _Transport(SemanticError("bad request", 400))
    bridge, _lim, _priv, _audit = _rig(tmp_path, t)
    with pytest.raises(SemanticError):
        bridge.generate("gpt", "hi")
    assert len(t.calls) == 1


def test_stream_interrupted_is_not_retried(tmp_path: Path) -> None:
    """Generation began — retrying doubles cost and may double a side effect.
    Costlier here than locally, which is why it must not drift."""
    t = _Transport(StreamInterrupted("stalled", "read_timeout"))
    bridge, _lim, _priv, _audit = _rig(tmp_path, t)
    with pytest.raises(StreamInterrupted):
        bridge.generate("gpt", "hi")
    assert len(t.calls) == 1


def test_deadline_covers_all_attempts(tmp_path: Path) -> None:
    clock = _Clock()
    t = _Transport(*[TransportError("refused", "connect") for _ in range(5)],
                   clock=clock, cost_s=20.0)
    bridge, _lim, _priv, _audit = _rig(tmp_path, t, clock=clock, deadline_s=30.0)
    with pytest.raises(DeadlineExceeded):
        bridge.generate("gpt", "hi")
    assert clock.now <= 30.0


# -- audit ---------------------------------------------------------------------
def test_successful_call_audits_tokens_and_latency(tmp_path: Path) -> None:
    t = _Transport(Response("hi", "gpt", prompt_tokens=5, completion_tokens=9))
    bridge, _lim, _priv, _audit = _rig(tmp_path, t)
    bridge.generate("gpt", "hi")
    calls = [e for e in _entries(tmp_path) if e["action"] == "cloud.call"]
    assert len(calls) == 1
    assert calls[0]["extra"]["token_count"] == 14
    assert calls[0]["extra"]["latency_ms"] >= 0.0


def test_prompt_is_hashed_never_logged(tmp_path: Path) -> None:
    """Off-machine calls are exactly where a leaked prompt costs most."""
    secret = "internal roadmap: acquire competitor in Q3"
    t = _Transport(Response("ok", "gpt", 1, 1))
    bridge, _lim, _priv, _audit = _rig(tmp_path, t)
    bridge.generate("gpt", secret)
    raw = (tmp_path / "audit_log.jsonl").read_text(encoding="utf-8")
    assert "acquire competitor" not in raw
    assert hashlib.sha256(secret.encode("utf-8")).hexdigest() in raw


def test_denial_is_audited_without_the_prompt(tmp_path: Path) -> None:
    bridge, _lim, _priv, _audit = _rig(tmp_path, principal=_AGENT)
    with pytest.raises(CapError):
        bridge.generate("gpt", "top secret plan")
    raw = (tmp_path / "audit_log.jsonl").read_text(encoding="utf-8")
    assert "top secret plan" not in raw
    assert any(e["action"] == "cloud.denied" for e in _entries(tmp_path))


def test_audit_shape_matches_the_local_bridge(tmp_path: Path) -> None:
    """Two transports with different audit shapes are two things to reason
    about, and the second is the one nobody re-reads."""
    t = _Transport(Response("ok", "gpt", 1, 1))
    bridge, _lim, _priv, _audit = _rig(tmp_path, t)
    bridge.generate("gpt", "hi")
    call = [e for e in _entries(tmp_path) if e["action"] == "cloud.call"][0]
    assert {"model", "endpoint", "attempt", "latency_ms", "total_latency_ms",
            "token_count", "prompt_sha256", "truncated"} <= set(call["extra"])


# -- input validation ----------------------------------------------------------
@pytest.mark.parametrize(("model", "prompt", "match"), [
    ("", "hi", "needs a model"),
    ("gpt", "", "needs a prompt"),
])
def test_malformed_calls_refused(tmp_path: Path, model, prompt, match) -> None:
    t = _Transport()
    bridge, _lim, _priv, _audit = _rig(tmp_path, t)
    with pytest.raises(CloudBridgeError, match=match):
        bridge.generate(model, prompt)
    assert t.calls == []


def test_no_live_endpoint_is_required(tmp_path: Path) -> None:
    """S9 — the module must import no HTTP or socket library."""
    import inspect

    from aether.cloud_bridge import transport as mod

    src = inspect.getsource(mod)
    for forbidden in ("import requests", "import httpx", "import urllib",
                      "import socket", "import http.client"):
        assert forbidden not in src
    sig = inspect.signature(CloudBridge.__init__)
    assert sig.parameters["transport"].default is inspect.Parameter.empty
