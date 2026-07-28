"""DDR-792 — Ollama transport bridge.

The single module permitted to speak the wire protocol to a local Ollama
server. I-01/I-08 forbids any module under ``aether/agents/`` from importing an
HTTP client; this is where that traffic legitimately lives.

Concentrating it is the point. Model traffic scattered across agents cannot be
rate-limited, cannot be audited uniformly, and cannot be blocked by privacy
mode, because each call site would have to remember all three.

Three rules do the work.

**Retry transport, never semantics.** A connection refused means the request
never ran; a 400 means the request is wrong and re-sending it sends the same
wrong request. The case that separates a real policy from a naive one is the
read timeout *after* first byte: the model is already generating, so a retry
doubles the cost and may double any side effect. It is not retried.

**The deadline covers everything.** 30 s is a budget for all attempts combined,
not per attempt. A retry policy that can exceed its own deadline is how a
"30 s timeout" becomes a 90 s stall.

**Privacy mode is checked before a socket exists.** Not after the response, not
at the caller — here, before the transport is touched, because a check the
caller performs is a check the next caller forgets.

The transport itself is injected (**S9**), so CI never needs a live Ollama.
"""

from __future__ import annotations

import hashlib
import random
import time
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, Callable, Protocol

from aether.kernel.audit.audit_log import AuditLog

__all__ = [
    "OllamaBridge",
    "BridgeError",
    "TransportError",
    "SemanticError",
    "DeadlineExceeded",
    "PrivacyModeViolation",
    "Transport",
    "Response",
    "RetryPolicy",
    "DEFAULT_DEADLINE_S",
    "MAX_ATTEMPTS",
]

DDR_ID = "DDR-792"
_FILE = "aether/ollama_bridge/transport.py"

DEFAULT_DEADLINE_S = 30.0
MAX_ATTEMPTS = 3
BASE_BACKOFF_S = 0.25
CONNECT_TIMEOUT_S = 5.0
FIRST_BYTE_TIMEOUT_S = 15.0


class BridgeError(RuntimeError):
    """Base for every bridge failure."""


class TransportError(BridgeError):
    """The request did not reach a served endpoint. Retryable."""

    def __init__(self, message: str, kind: str = "connect") -> None:
        super().__init__(message)
        self.kind = kind


class SemanticError(BridgeError):
    """The server answered and the answer is a refusal. NOT retryable."""

    def __init__(self, message: str, status: int) -> None:
        super().__init__(message)
        self.status = status


class DeadlineExceeded(BridgeError):
    """The 30 s budget ran out across all attempts."""


class PrivacyModeViolation(BridgeError):
    """Privacy mode is active; no outbound call may be made."""


class StreamInterrupted(TransportError):
    """Generation started and then stalled. **Not** retried — see module doc."""


@dataclass(slots=True)
class Response:
    text: str
    model: str
    prompt_tokens: int = 0
    completion_tokens: int = 0
    truncated: bool = False

    @property
    def token_count(self) -> int:
        return self.prompt_tokens + self.completion_tokens


class Transport(Protocol):
    """The wire. Injected so CI never needs a live server (S9)."""

    def send(
        self, endpoint: str, model: str, prompt: str, timeout_s: float
    ) -> Response: ...


@dataclass(slots=True)
class RetryPolicy:
    max_attempts: int = MAX_ATTEMPTS
    base_backoff_s: float = BASE_BACKOFF_S
    #: Full jitter. Not cosmetic: without it, N agents that fail together retry
    #: together and synchronise into a herd against one local server.
    jitter: Callable[[float], float] = field(
        default_factory=lambda: (lambda hi: random.uniform(0.0, hi))
    )

    def backoff_for(self, attempt: int) -> float:
        """Full-jitter exponential backoff for a 1-based attempt number."""
        ceiling = self.base_backoff_s * (2 ** max(0, attempt - 1))
        return self.jitter(ceiling)


def _sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


class OllamaBridge:
    """Bounded, audited, privacy-aware transport to a local Ollama server."""

    def __init__(
        self,
        transport: Transport,
        endpoint: str = "127.0.0.1:11434",
        audit: AuditLog | None = None,
        deadline_s: float = DEFAULT_DEADLINE_S,
        retry: RetryPolicy | None = None,
        clock: Callable[[], float] | None = None,
        sleep: Callable[[float], None] | None = None,
        privacy_fn: Callable[[], bool] | None = None,
        rate_limiter: Any | None = None,
        alignment_tap: Callable[[str, dict[str, Any]], None] | None = None,
    ) -> None:
        self.transport = transport
        self.endpoint = endpoint
        self.audit = audit if audit is not None else AuditLog()
        self.deadline_s = float(deadline_s)
        self.retry = retry if retry is not None else RetryPolicy()
        self.clock = clock if clock is not None else time.monotonic
        self.sleep = sleep if sleep is not None else time.sleep
        #: Callable rather than a flag: privacy mode can be toggled at runtime,
        #: and a value captured at construction would go stale exactly when it
        #: matters.
        self.privacy_fn = privacy_fn
        self.rate_limiter = rate_limiter
        self.alignment_tap = alignment_tap

    def _emit(self, step: str, detail: dict[str, Any]) -> None:
        if self.alignment_tap is not None:
            self.alignment_tap(f"I-10.{step}", detail)

    # -- the call ------------------------------------------------------------
    def generate(self, model: str, prompt: str) -> Response:
        """One completion, within the deadline, audited, privacy-checked."""
        if not model:
            raise BridgeError("a call needs a model")
        if not prompt:
            raise BridgeError("a call needs a prompt")

        # Privacy first — BEFORE the rate limiter and before any transport
        # touch, so a blocked call consumes no quota and opens no socket.
        if self.privacy_fn is not None and self.privacy_fn():
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="ollama.blocked",
                gate_status="privacy", model=model,
                prompt_sha256=_sha256(prompt),
            )
            self._emit("privacy_blocked", {"model": model})
            raise PrivacyModeViolation(
                "privacy mode is active; outbound model calls are blocked"
            )

        if self.rate_limiter is not None:
            self.rate_limiter.acquire("ollama")

        started = self.clock()
        last: BridgeError | None = None

        for attempt in range(1, self.retry.max_attempts + 1):
            remaining = self.deadline_s - (self.clock() - started)
            if remaining <= 0:
                raise self._deadline(model, prompt, started, attempt)

            attempt_started = self.clock()
            try:
                response = self.transport.send(
                    self.endpoint, model, prompt,
                    timeout_s=min(remaining, FIRST_BYTE_TIMEOUT_S),
                )
            except StreamInterrupted as exc:
                # Generation had already begun. Retrying doubles cost and may
                # double a side effect, so this terminates the call.
                self._audit_attempt(model, prompt, attempt, attempt_started,
                                    "failed", "stream_interrupted")
                raise
            except SemanticError as exc:
                # The server answered. Re-sending sends the same wrong request.
                self._audit_attempt(model, prompt, attempt, attempt_started,
                                    "refused", f"http_{exc.status}")
                self._emit("semantic_error", {"status": exc.status})
                raise
            except TransportError as exc:
                last = exc
                self._audit_attempt(model, prompt, attempt, attempt_started,
                                    "retried", exc.kind)
                if attempt >= self.retry.max_attempts:
                    break
                backoff = self.retry.backoff_for(attempt)
                elapsed = self.clock() - started
                if elapsed + backoff >= self.deadline_s:
                    # Sleeping would blow the budget; fail at the deadline
                    # rather than after it.
                    raise self._deadline(model, prompt, started, attempt) from exc
                self.sleep(backoff)
                continue

            total_ms = (self.clock() - started) * 1000.0
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="ollama.call", gate_status="ok",
                model=model, endpoint=self.endpoint, attempt=attempt,
                latency_ms=round((self.clock() - attempt_started) * 1000.0, 3),
                total_latency_ms=round(total_ms, 3),
                token_count=response.token_count,
                prompt_sha256=_sha256(prompt),
                truncated=response.truncated,
            )
            self._emit("call", {"model": model, "attempt": attempt})
            return response

        assert last is not None
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="ollama.failed",
            gate_status="failed", model=model, endpoint=self.endpoint,
            attempts=self.retry.max_attempts, error_kind=last.kind,
            prompt_sha256=_sha256(prompt),
            total_latency_ms=round((self.clock() - started) * 1000.0, 3),
        )
        self._emit("failed", {"model": model})
        raise last

    # -- helpers -------------------------------------------------------------
    def _audit_attempt(
        self, model: str, prompt: str, attempt: int, attempt_started: float,
        status: str, error_kind: str,
    ) -> None:
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="ollama.retry", gate_status=status,
            model=model, endpoint=self.endpoint, attempt=attempt,
            latency_ms=round((self.clock() - attempt_started) * 1000.0, 3),
            error_kind=error_kind, prompt_sha256=_sha256(prompt),
        )

    def _deadline(
        self, model: str, prompt: str, started: float, attempt: int
    ) -> DeadlineExceeded:
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="ollama.failed",
            gate_status="timeout", model=model, endpoint=self.endpoint,
            attempts=attempt, error_kind="deadline",
            prompt_sha256=_sha256(prompt),
            total_latency_ms=round((self.clock() - started) * 1000.0, 3),
        )
        self._emit("deadline", {"model": model})
        return DeadlineExceeded(
            f"{self.deadline_s}s deadline exhausted after {attempt} attempt(s)"
        )
