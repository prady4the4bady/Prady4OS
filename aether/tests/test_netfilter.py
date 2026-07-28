"""CONFIRM-1 gate (3) — privacy-mode netfilter hook."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.kernel.audit.audit_log import AuditLog
from aether.ollama_bridge.transport import OllamaBridge, Response, RetryPolicy
from aether.platform.privacy.netfilter import (
    NetFilter,
    PrivacyMode,
    PrivacyModeViolation,
    filtered_transport,
)


class _Inner:
    """The transport that must never be reached while privacy is on."""

    def __init__(self) -> None:
        self.sent: list[str] = []

    def send(self, endpoint: str, model: str, prompt: str,
             timeout_s: float) -> Response:
        self.sent.append(endpoint)
        return Response("leaked", model, 1, 1)


def _filter(tmp_path: Path, privacy: PrivacyMode, tap=None) -> NetFilter:
    return NetFilter(privacy=privacy,
                     audit=AuditLog(tmp_path / "audit_log.jsonl"),
                     alignment_tap=tap)


def test_privacy_on_blocks_outbound_calls(tmp_path: Path) -> None:
    """THE gate — privacy=True must block."""
    privacy = PrivacyMode()
    privacy.enable()
    nf = _filter(tmp_path, privacy)
    with pytest.raises(PrivacyModeViolation, match="blocked"):
        nf.check("api.example.com:443")


def test_privacy_off_passes_through(tmp_path: Path) -> None:
    """privacy=False must pass through — a filter that blocks everything is
    not a privacy mode, it is an outage."""
    nf = _filter(tmp_path, PrivacyMode())
    nf.check("api.example.com:443")
    assert nf.allowed_count == 1


def test_interception_is_at_the_transport_not_the_caller(tmp_path: Path) -> None:
    """THE discriminator.

    A flag check in the caller is a check the next caller forgets — and the next
    caller is exactly the path nobody reviewed. Wrapping the transport means a
    call site that never heard of privacy mode is blocked anyway.

    Here the caller does no privacy check at all, and the inner transport must
    still never be reached.
    """
    privacy = PrivacyMode()
    privacy.enable()
    inner = _Inner()
    wrapped = filtered_transport(inner=inner, netfilter=_filter(tmp_path, privacy))

    with pytest.raises(PrivacyModeViolation):
        wrapped.send("api.example.com:443", "llama3", "hi", timeout_s=5.0)

    assert inner.sent == [], "the transport was reached despite privacy mode"
    assert wrapped.calls_attempted == ["api.example.com:443"]


def test_block_happens_before_the_socket_not_after_the_response(
    tmp_path: Path,
) -> None:
    """A check that runs after the call has not prevented a leak — it has
    discovered one."""
    privacy = PrivacyMode()
    privacy.enable()
    inner = _Inner()
    wrapped = filtered_transport(inner=inner, netfilter=_filter(tmp_path, privacy))
    with pytest.raises(PrivacyModeViolation):
        wrapped.send("evil.example.com:443", "llama3", "secret", timeout_s=1.0)
    assert inner.sent == []


def test_toggling_privacy_takes_effect_immediately(tmp_path: Path) -> None:
    """State captured at construction is stale exactly when the operator most
    needs it current."""
    privacy = PrivacyMode()
    inner = _Inner()
    wrapped = filtered_transport(inner=inner, netfilter=_filter(tmp_path, privacy))

    wrapped.send("api.example.com:443", "llama3", "hi", timeout_s=1.0)
    assert inner.sent == ["api.example.com:443"]

    privacy.enable()
    with pytest.raises(PrivacyModeViolation):
        wrapped.send("api.example.com:443", "llama3", "hi", timeout_s=1.0)
    assert len(inner.sent) == 1

    privacy.disable()
    wrapped.send("api.example.com:443", "llama3", "hi", timeout_s=1.0)
    assert len(inner.sent) == 2


def test_loopback_is_not_exempt_by_default(tmp_path: Path) -> None:
    """'Local' is a property of an address, not of a destination: a forwarded
    port or a proxy on loopback reaches the internet. Exempting it silently is
    how privacy mode becomes advisory."""
    privacy = PrivacyMode()
    privacy.enable()
    nf = _filter(tmp_path, privacy)
    for local in ("127.0.0.1:11434", "localhost:11434", "[::1]:11434"):
        with pytest.raises(PrivacyModeViolation):
            nf.check(local)


def test_loopback_exemption_is_explicit_and_narrow(tmp_path: Path) -> None:
    privacy = PrivacyMode()
    privacy.enable()
    privacy.allow_loopback()
    nf = _filter(tmp_path, privacy)

    nf.check("127.0.0.1:11434")            # exempted
    with pytest.raises(PrivacyModeViolation):
        nf.check("api.example.com:443")    # and only loopback


def test_hostname_that_merely_looks_local_is_still_blocked(tmp_path: Path) -> None:
    """'localhost.evil.com' is not loopback. A substring check would exempt it."""
    privacy = PrivacyMode()
    privacy.enable()
    privacy.allow_loopback()
    nf = _filter(tmp_path, privacy)
    for spoof in ("localhost.evil.com:443", "127.0.0.1.evil.com:443",
                  "notlocalhost:443"):
        with pytest.raises(PrivacyModeViolation):
            nf.check(spoof)


def test_blocks_are_audited_with_the_destination(tmp_path: Path) -> None:
    privacy = PrivacyMode()
    privacy.enable()
    nf = _filter(tmp_path, privacy)
    with pytest.raises(PrivacyModeViolation):
        nf.check("api.example.com:443")
    blocks = [e for e in AuditLog(tmp_path / "audit_log.jsonl").read_all()
              if e["action"] == "netfilter.block"]
    assert len(blocks) == 1
    assert blocks[0]["extra"]["destination"] == "api.example.com:443"
    assert blocks[0]["gate_status"] == "blocked"


def test_allowals_are_audited_too(tmp_path: Path) -> None:
    nf = _filter(tmp_path, PrivacyMode())
    nf.check("api.example.com:443")
    assert any(e["action"] == "netfilter.allow"
               for e in AuditLog(tmp_path / "audit_log.jsonl").read_all())


def test_evaluate_is_a_probe_and_does_not_raise(tmp_path: Path) -> None:
    privacy = PrivacyMode()
    privacy.enable()
    nf = _filter(tmp_path, privacy)
    verdict = nf.evaluate("api.example.com:443")
    assert verdict.allowed is False
    assert verdict.reason
    assert nf.blocked_count == 0, "a probe must not count as an enforcement"


def test_end_to_end_through_the_ollama_bridge(tmp_path: Path) -> None:
    """The real wire: DDR-792's bridge over a filtered transport.

    The bridge's own privacy_fn is deliberately NOT set here — this proves the
    netfilter blocks on its own, so a bridge (or any future caller) that forgot
    to wire privacy_fn is still contained.
    """
    privacy = PrivacyMode()
    privacy.enable()
    inner = _Inner()
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    wrapped = filtered_transport(
        inner=inner, netfilter=NetFilter(privacy=privacy, audit=audit))

    bridge = OllamaBridge(
        transport=wrapped, audit=audit, retry=RetryPolicy(jitter=lambda hi: hi),
        sleep=lambda _s: None,
    )
    with pytest.raises(PrivacyModeViolation):
        bridge.generate("llama3", "hi")
    assert inner.sent == []


def test_privacy_mode_records_when_it_was_enabled(tmp_path: Path) -> None:
    """An incident review needs to know whether privacy was on at the time."""
    privacy = PrivacyMode()
    assert privacy.since_ts == 0.0
    privacy.enable(clock=lambda: 1234.0)
    assert privacy.active is True
    assert privacy.since_ts == 1234.0
    privacy.disable()
    assert privacy.since_ts == 0.0


def test_alignment_tap_receives_steps(tmp_path: Path) -> None:
    events: list[str] = []
    privacy = PrivacyMode()
    nf = _filter(tmp_path, privacy, tap=lambda s, _d: events.append(s))
    nf.check("api.example.com:443")
    privacy.enable()
    with pytest.raises(PrivacyModeViolation):
        nf.check("api.example.com:443")
    assert {"F-3D.allow", "F-3D.block"} <= set(events)
