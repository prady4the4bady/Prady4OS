"""CONFIRM-1 gate (3) — privacy-mode netfilter hook.

When privacy mode is active, **no outbound call leaves the machine.**

The design point that makes this worth having: the block is installed at the
**transport boundary**, by wrapping the transport itself, not by asking each
caller to check a flag first. A flag check in the caller is a check the next
caller forgets, and "the next caller" is exactly the code path nobody reviewed —
a new agent, a retry helper, a debug script. Wrapping the transport means a call
site that never heard of privacy mode is blocked anyway, because the object it
holds is the filter.

Two further properties, both learned from how this fails in practice:

* **Blocked before the socket, not after the response.** A check that runs after
  the call has already sent the data has not prevented anything; it has only
  discovered the leak.
* **State is read at call time.** Privacy mode is toggled at runtime, so a value
  captured at construction is stale exactly when the operator most needs it to
  be current.

Local-loopback traffic is **not** exempt. It is tempting — the local Ollama
server is on 127.0.0.1 and blocking it makes privacy mode look like it broke the
machine. But "local" is a property of an address, not of a destination: a
forwarded port or a proxy on loopback reaches the internet. Exemptions are the
operator's to grant explicitly via :meth:`PrivacyMode.allow_loopback`, never the
default.
"""

from __future__ import annotations

import ipaddress
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Protocol

from aether.kernel.audit.audit_log import AuditLog

__all__ = [
    "PrivacyMode",
    "PrivacyModeViolation",
    "NetFilter",
    "FilterVerdict",
    "filtered_transport",
]

DDR_ID = "F-3D"
_FILE = "aether/platform/privacy/netfilter.py"


class PrivacyModeViolation(RuntimeError):
    """Privacy mode is active and an outbound call was attempted."""


@dataclass(slots=True)
class FilterVerdict:
    allowed: bool
    reason: str
    destination: str = ""


@dataclass(slots=True)
class PrivacyMode:
    """Runtime privacy state. Mutable, read at call time — never cached."""

    active: bool = False
    #: Loopback is NOT exempt by default. See the module docstring.
    loopback_exempt: bool = False
    since_ts: float = 0.0

    def enable(self, clock: Callable[[], float] = time.time) -> None:
        self.active = True
        self.since_ts = clock()

    def disable(self) -> None:
        self.active = False
        self.since_ts = 0.0

    def allow_loopback(self) -> None:
        """Explicit operator exemption for loopback destinations."""
        self.loopback_exempt = True


def _is_loopback(destination: str) -> bool:
    host = destination.rsplit(":", 1)[0] if ":" in destination else destination
    host = host.strip("[]")
    if host in ("localhost", ""):
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


class NetFilter:
    """The hook. Decides, audits, and refuses — in that order."""

    def __init__(
        self,
        privacy: PrivacyMode,
        audit: AuditLog | None = None,
        alignment_tap: Callable[[str, dict[str, Any]], None] | None = None,
    ) -> None:
        self.privacy = privacy
        self.audit = audit if audit is not None else AuditLog()
        self.alignment_tap = alignment_tap
        self.blocked_count = 0
        self.allowed_count = 0

    def _emit(self, step: str, detail: dict[str, Any]) -> None:
        if self.alignment_tap is not None:
            self.alignment_tap(f"{DDR_ID}.{step}", detail)

    def evaluate(self, destination: str) -> FilterVerdict:
        """Decide without acting. Used for probes and for the wrapper below."""
        if not self.privacy.active:
            return FilterVerdict(True, "privacy mode inactive", destination)
        if self.privacy.loopback_exempt and _is_loopback(destination):
            return FilterVerdict(True, "loopback exempted by operator",
                                 destination)
        return FilterVerdict(False, "privacy mode active", destination)

    def check(self, destination: str) -> None:
        """Authorise an outbound call, or raise before anything is sent."""
        verdict = self.evaluate(destination)
        if verdict.allowed:
            self.allowed_count += 1
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="netfilter.allow",
                gate_status="allowed", destination=destination,
                reason=verdict.reason,
            )
            self._emit("allow", {"destination": destination})
            return
        self.blocked_count += 1
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="netfilter.block",
            gate_status="blocked", destination=destination,
            reason=verdict.reason,
        )
        self._emit("block", {"destination": destination})
        raise PrivacyModeViolation(
            f"privacy mode is active; outbound call to {destination!r} blocked"
        )


class _Transport(Protocol):
    def send(self, endpoint: str, model: str, prompt: str,
             timeout_s: float) -> Any: ...


@dataclass(slots=True)
class filtered_transport:      # noqa: N801 — reads as a factory at call sites
    """Wrap a transport so the filter runs before it, unconditionally.

    This is the interception point. A caller holding one of these cannot skip
    the check, because there is no path to the inner transport that does not go
    through :meth:`send`.
    """

    inner: _Transport
    netfilter: NetFilter
    calls_attempted: list[str] = field(default_factory=list)

    def send(self, endpoint: str, model: str, prompt: str,
             timeout_s: float) -> Any:
        self.calls_attempted.append(endpoint)
        # Before the inner transport is touched. A check after this line would
        # discover the leak rather than prevent it.
        self.netfilter.check(endpoint)
        return self.inner.send(endpoint, model, prompt, timeout_s)
