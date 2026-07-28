"""Cloud/hybrid inference bridge (DDR-793, unblocked once CONFIRM-1's four
gates were met).

Shaped after DDR-792's ollama bridge deliberately — same retry semantics, same
audit schema, same deadline discipline — because two transports that behave
differently are two things to reason about, and the second one is always the
one nobody re-reads.

What differs is what is **mandatory** rather than optional:

* ``rate_limiter`` is **required**. In the local bridge it is optional, because
  a missing limiter there costs a burst against a process on the same machine.
  A cloud bridge without a limiter *is* the bypass CONFIRM-1 gate (4) forbids —
  so it is a constructor argument with no default, and an ``OllamaBridge``-style
  ``None`` is refused rather than tolerated.
* ``netfilter`` is **required**. Privacy mode's whole purpose is stopping data
  leaving the machine; an optional privacy filter on the one component that
  exists to send data off-machine is not a safety control, it is a suggestion.
* ``enforcer`` + ``principal`` are **required**. Reaching an unlisted host is
  the ``CAP_NET_BROWSE`` case, and I-02 answers "may this principal".

**Not enabled by default anywhere.** DDR-794 concluded that `CAP_NET_BROWSE`
carries two unresolved kernel-side risks — R1, the sovereign bypass is total,
and R3, the allowlist match path is not audited per destination. This module is
built and gated so the Python half is ready and reviewable; it must not be wired
into a shipping configuration until R1 and R3 are closed. That constraint is
recorded in DDR-793 and restated here because the file, not the doc, is what the
next person reads.
"""

from __future__ import annotations

import hashlib
import time
from dataclasses import dataclass
from typing import Any, Callable, Protocol

from aether.capability.enforcement import CapabilityEnforcer, CapError, Principal
from aether.kernel.audit.audit_log import AuditLog
from aether.ollama_bridge.transport import (
    BridgeError,
    DeadlineExceeded,
    Response,
    RetryPolicy,
    SemanticError,
    StreamInterrupted,
    TransportError,
)
from aether.platform.privacy.netfilter import NetFilter
from aether.platform.ratelimit.shared_limiter import SharedRateLimiter

__all__ = [
    "CloudBridge",
    "CloudBridgeError",
    "BROWSE_OPERATION",
    "DEFAULT_DEADLINE_S",
]

DDR_ID = "DDR-793"
_FILE = "aether/cloud_bridge/transport.py"

BROWSE_OPERATION = "net.browse"
DEFAULT_DEADLINE_S = 30.0
MAX_ATTEMPTS = 3
RATE_PATH = "cloud"


class CloudBridgeError(BridgeError):
    """Cloud bridge misconfigured or refused."""


class CloudTransport(Protocol):
    def send(self, endpoint: str, model: str, prompt: str,
             timeout_s: float) -> Response: ...


def _sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


@dataclass(slots=True)
class _Attempt:
    number: int
    started: float


class CloudBridge:
    """Off-machine inference, behind every control at once."""

    def __init__(
        self,
        transport: CloudTransport,
        enforcer: CapabilityEnforcer,
        principal: Principal,
        rate_limiter: SharedRateLimiter,
        netfilter: NetFilter,
        endpoint: str = "api.example.com:443",
        audit: AuditLog | None = None,
        deadline_s: float = DEFAULT_DEADLINE_S,
        retry: RetryPolicy | None = None,
        clock: Callable[[], float] | None = None,
        sleep: Callable[[float], None] | None = None,
        alignment_tap: Callable[[str, dict[str, Any]], None] | None = None,
    ) -> None:
        # Refused rather than defaulted: a cloud bridge missing any one of these
        # is precisely the configuration CONFIRM-1 blocks.
        if rate_limiter is None:
            raise CloudBridgeError("cloud_bridge requires the shared rate limiter")
        if netfilter is None:
            raise CloudBridgeError("cloud_bridge requires the privacy netfilter")
        if enforcer is None or principal is None:
            raise CloudBridgeError("cloud_bridge requires a principal and enforcer")

        self.transport = transport
        self.enforcer = enforcer
        self.principal = principal
        self.rate_limiter = rate_limiter
        self.netfilter = netfilter
        self.endpoint = endpoint
        self.audit = audit if audit is not None else AuditLog()
        self.deadline_s = float(deadline_s)
        self.retry = retry if retry is not None else RetryPolicy()
        self.clock = clock if clock is not None else time.monotonic
        self.sleep = sleep if sleep is not None else time.sleep
        self.alignment_tap = alignment_tap

    def _emit(self, step: str, detail: dict[str, Any]) -> None:
        if self.alignment_tap is not None:
            self.alignment_tap(f"{DDR_ID}.{step}", detail)

    # -- the call ------------------------------------------------------------
    def generate(self, model: str, prompt: str) -> Response:
        """One off-machine completion, through every control in order."""
        if not model:
            raise CloudBridgeError("a call needs a model")
        if not prompt:
            raise CloudBridgeError("a call needs a prompt")

        # 1. Authority. Asked first because a principal that may not browse
        #    should not consume budget or reach the filter at all.
        try:
            self.enforcer.check(self.principal, BROWSE_OPERATION,
                                resource=self.endpoint)
        except CapError:
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="cloud.denied",
                gate_status="denied", principal=self.principal.principal_id,
                endpoint=self.endpoint, prompt_sha256=_sha256(prompt),
            )
            self._emit("denied", {"principal": self.principal.principal_id})
            raise

        # 2. Privacy. Before the limiter, so a blocked call spends no budget.
        self.netfilter.check(self.endpoint)

        # 3. The SHARED budget — same counter as the local path (gate 4).
        self.rate_limiter.acquire(RATE_PATH)

        started = self.clock()
        last: BridgeError | None = None

        for attempt in range(1, self.retry.max_attempts + 1):
            remaining = self.deadline_s - (self.clock() - started)
            if remaining <= 0:
                raise self._deadline(model, prompt, started, attempt)

            attempt_started = self.clock()
            try:
                response = self.transport.send(
                    self.endpoint, model, prompt, timeout_s=remaining,
                )
            except StreamInterrupted:
                # Same rule as DDR-792: generation began, so a retry doubles
                # cost and may double a side effect.
                self._audit_attempt(model, prompt, attempt, attempt_started,
                                    "failed", "stream_interrupted")
                raise
            except SemanticError as exc:
                self._audit_attempt(model, prompt, attempt, attempt_started,
                                    "refused", f"http_{exc.status}")
                raise
            except TransportError as exc:
                last = exc
                self._audit_attempt(model, prompt, attempt, attempt_started,
                                    "retried", exc.kind)
                if attempt >= self.retry.max_attempts:
                    break
                backoff = self.retry.backoff_for(attempt)
                if (self.clock() - started) + backoff >= self.deadline_s:
                    raise self._deadline(model, prompt, started, attempt) from exc
                self.sleep(backoff)
                continue

            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="cloud.call", gate_status="ok",
                model=model, endpoint=self.endpoint, attempt=attempt,
                latency_ms=round((self.clock() - attempt_started) * 1000.0, 3),
                total_latency_ms=round((self.clock() - started) * 1000.0, 3),
                token_count=response.token_count,
                prompt_sha256=_sha256(prompt),
                truncated=response.truncated,
            )
            self._emit("call", {"model": model, "attempt": attempt})
            return response

        assert last is not None
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="cloud.failed", gate_status="failed",
            model=model, endpoint=self.endpoint,
            attempts=self.retry.max_attempts, error_kind=last.kind,
            prompt_sha256=_sha256(prompt),
        )
        self._emit("failed", {"model": model})
        raise last

    # -- helpers -------------------------------------------------------------
    def _audit_attempt(
        self, model: str, prompt: str, attempt: int, attempt_started: float,
        status: str, error_kind: str,
    ) -> None:
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="cloud.retry", gate_status=status,
            model=model, endpoint=self.endpoint, attempt=attempt,
            latency_ms=round((self.clock() - attempt_started) * 1000.0, 3),
            error_kind=error_kind, prompt_sha256=_sha256(prompt),
        )

    def _deadline(
        self, model: str, prompt: str, started: float, attempt: int
    ) -> DeadlineExceeded:
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="cloud.failed", gate_status="timeout",
            model=model, endpoint=self.endpoint, attempts=attempt,
            error_kind="deadline", prompt_sha256=_sha256(prompt),
        )
        self._emit("deadline", {"model": model})
        return DeadlineExceeded(
            f"{self.deadline_s}s deadline exhausted after {attempt} attempt(s)"
        )
