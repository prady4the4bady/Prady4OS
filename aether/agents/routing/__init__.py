"""Privacy mode (ring-3) and model routing (Section 3D #58/#59, DDR-852).

Spec: *"privacy mode (blocks non-local Ollama via lwIP netfilter hook) · model
routing"*.

**The kernel is the enforcement. This is not.** `kernel/aether/` already blocks
non-local egress in privacy mode and records `AR_PRIVACY_BLOCKED` (DDR-802,
gate `smoke-privacy-netfilter`). What ring-3 adds is an *early, readable*
refusal: an agent that routes to a remote model in privacy mode should be told
why by the router, not discover it as an opaque `-EPERM` three layers down with
the reason recorded somewhere it is not reading.

Saying that plainly matters, because a ring-3 check that *looks* like
enforcement invites someone to weaken the kernel one on the grounds that it is
redundant. It is not redundant; it is the only one that counts. This layer is a
convenience that must never be trusted.

Which is why it **fails closed**: if privacy state cannot be determined, it is
treated as ON. The alternative — assume off when unsure — turns an unreadable
config into an open egress path, and nothing in the logs would say so.
"""

from __future__ import annotations

import ipaddress
from dataclasses import dataclass
from enum import Enum

__all__ = [
    "RoutingError",
    "PrivacyBlocked",
    "Endpoint",
    "ModelSpec",
    "Router",
    "is_local_host",
    "PrivacyState",
]


class RoutingError(Exception):
    """A route that must not be taken."""


class PrivacyBlocked(RoutingError):
    """Refused because privacy mode is on. Distinct so it is countable.

    Deliberately a separate type rather than a flag on RoutingError: folding it
    in makes "did privacy mode actually stop anything?" unanswerable, which is
    the same reasoning that gave `AR_PRIVACY_BLOCKED` its own audit code
    (kernel/aether/aether.h, DDR-802).
    """


class PrivacyState(Enum):
    ON = "on"
    OFF = "off"
    UNKNOWN = "unknown"

    @property
    def blocks_remote(self) -> bool:
        """UNKNOWN blocks. An unreadable config must not open an egress path."""
        return self is not PrivacyState.OFF


def is_local_host(host: str) -> bool:
    """True only for loopback and unambiguously link-local/private addresses.

    Hostnames that are not literal addresses return **False**, including ones
    that look local. Resolution happens elsewhere and can change between the
    check and the connection; treating an unresolved name as local would make
    the classification depend on a DNS answer this function never saw.
    """
    if not host or not host.strip():
        return False
    name = host.strip().lower()
    if name in ("localhost", "localhost.localdomain"):
        return True
    try:
        addr = ipaddress.ip_address(name)
    except ValueError:
        return False
    return bool(addr.is_loopback or addr.is_private or addr.is_link_local)


@dataclass(frozen=True)
class Endpoint:
    """Where a model is served. Host is classified, never pattern-matched."""

    host: str
    port: int

    def __post_init__(self) -> None:
        if not 0 < self.port < 65536:
            raise RoutingError(f"port out of range: {self.port}")
        if not self.host.strip():
            raise RoutingError("endpoint requires a host")

    @property
    def is_local(self) -> bool:
        return is_local_host(self.host)


@dataclass(frozen=True)
class ModelSpec:
    """A routable model.

    `quality` and `est_cost_microcents_per_1k` are both required. A model with
    no declared cost cannot be compared against a budget, and defaulting it to
    zero would make the cheapest option always be the one nobody has measured.
    """

    name: str
    endpoint: Endpoint
    quality: int
    est_cost_microcents_per_1k: int
    context_window: int

    def __post_init__(self) -> None:
        if not self.name.strip():
            raise RoutingError("model requires a name")
        if self.quality < 0 or self.est_cost_microcents_per_1k < 0:
            raise RoutingError(f"{self.name}: quality and cost cannot be negative")
        if self.context_window <= 0:
            raise RoutingError(f"{self.name}: context_window must be positive")


class Router:
    """Chooses a model. Refuses rather than degrading, like everything else."""

    def __init__(self, models: list[ModelSpec] | None = None) -> None:
        self._models: dict[str, ModelSpec] = {}
        for m in models or []:
            self.register(m)

    def register(self, model: ModelSpec) -> None:
        if model.name in self._models:
            raise RoutingError(
                f"model {model.name!r} already registered — two specs under one "
                "name means routing depends on which was loaded last"
            )
        self._models[model.name] = model

    @property
    def models(self) -> tuple[ModelSpec, ...]:
        return tuple(self._models.values())

    def check_allowed(self, model: ModelSpec, privacy: PrivacyState) -> None:
        """Raise if this model may not be reached under `privacy`."""
        if privacy.blocks_remote and not model.endpoint.is_local:
            raise PrivacyBlocked(
                f"{model.name} is served from {model.endpoint.host}, which is "
                f"not local, and privacy mode is {privacy.value}. "
                "(UNKNOWN blocks: an unreadable privacy state must not open an "
                "egress path.) The kernel netfilter hook is the enforcement — "
                "this refusal is the early, readable one"
            )

    def route(
        self,
        *,
        required_tokens: int,
        privacy: PrivacyState,
        budget_microcents: int | None = None,
        min_quality: int = 0,
    ) -> ModelSpec:
        """Pick the cheapest model meeting every constraint.

        Ties break on higher quality, then on name — deterministic, so the same
        request routes the same way. A router whose choice depends on dict order
        makes a bad answer impossible to attribute to a model.

        Raises rather than returning a second-best that violates a constraint:
        a caller handed a model that does not fit the context would discover it
        as a truncation, which is the failure this project refuses everywhere.
        """
        if required_tokens <= 0:
            raise RoutingError("required_tokens must be positive")
        if not self._models:
            raise RoutingError("no models registered")

        rejected: list[str] = []
        viable: list[ModelSpec] = []
        for m in sorted(self._models.values(), key=lambda m: m.name):
            try:
                self.check_allowed(m, privacy)
            except PrivacyBlocked as exc:
                rejected.append(f"{m.name}: {exc.__class__.__name__}")
                continue
            if m.context_window < required_tokens:
                rejected.append(
                    f"{m.name}: context {m.context_window} < {required_tokens}"
                )
                continue
            if m.quality < min_quality:
                rejected.append(f"{m.name}: quality {m.quality} < {min_quality}")
                continue
            if budget_microcents is not None:
                est = (required_tokens * m.est_cost_microcents_per_1k + 999) // 1000
                if est > budget_microcents:
                    rejected.append(f"{m.name}: est {est} > budget {budget_microcents}")
                    continue
            viable.append(m)

        if not viable:
            raise RoutingError(
                "no model satisfies the request. Rejected: "
                + "; ".join(rejected)
                + ". Refusing rather than returning a near-miss: a caller handed "
                "a model that does not fit would discover it as a truncation"
            )

        return min(
            viable,
            key=lambda m: (m.est_cost_microcents_per_1k, -m.quality, m.name),
        )
