"""B-17 — blind-spot discovery loop.

A blind spot is not a known failure — B-16 already tracks those. It is a region
the system has **never tested**, so no evidence exists either way. The loop's job
is to make absence of evidence visible, because a self-model built only from
observed outcomes is silently confident about everything it never tried.

Three kinds are surfaced:

* **untested** — a declared capability with no observations at all;
* **stale** — evidence exists but is older than the freshness window;
* **unclaimed** — a lockbox grant that the self-model never claimed, i.e. a
  capability the system holds and does not know it holds.

The third is the interesting one and the reason this reads the lockbox rather
than only the self-model.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable

from aether.agents.introspection.failure_analysis import FailureAnalyser
from aether.agents.self_model.self_model import SelfModel
from aether.kernel.audit.audit_log import AuditLog

__all__ = [
    "BlindSpotKind",
    "BlindSpot",
    "BlindSpotLoop",
    "DEFAULT_STALE_AFTER_S",
]

DDR_ID = "B-17"
_FILE = "aether/agents/blindspot/discovery_loop.py"

DEFAULT_STALE_AFTER_S = 30 * 24 * 3600.0          # 30 days


class BlindSpotKind(str, Enum):
    UNTESTED = "untested"
    STALE = "stale"
    UNCLAIMED = "unclaimed"


@dataclass(slots=True)
class BlindSpot:
    kind: BlindSpotKind
    subject: str
    reason: str
    severity: int = 1                 # 1..5
    context: dict[str, Any] = field(default_factory=dict)


class BlindSpotLoop:
    """Finds regions with no evidence, rather than regions with bad evidence."""

    def __init__(
        self,
        self_model: SelfModel,
        analyser: FailureAnalyser | None = None,
        audit: AuditLog | None = None,
        stale_after_s: float = DEFAULT_STALE_AFTER_S,
        now_fn: Callable[[], float] | None = None,
    ) -> None:
        self.self_model = self_model
        self.analyser = analyser if analyser is not None else FailureAnalyser()
        self.audit = audit if audit is not None else AuditLog()
        self.stale_after_s = float(stale_after_s)
        # Injected clock: a loop whose staleness rule cannot be tested without
        # waiting 30 days is a loop whose staleness rule is never tested.
        self.now_fn: Callable[[], float] = now_fn if now_fn is not None else time.time

    def discover(self) -> list[BlindSpot]:
        """Enumerate every region with no usable evidence."""
        now = self.now_fn()
        spots: list[BlindSpot] = []

        claimed_caps: set[tuple[str, str]] = set()
        for claim in self.self_model.claims:
            claimed_caps.add((claim.ddr_id, claim.capability))
            if claim.observations == 0:
                spots.append(
                    BlindSpot(
                        kind=BlindSpotKind.UNTESTED,
                        subject=claim.name,
                        reason="capability claimed but never exercised",
                        severity=3,
                        context={"ddr": claim.ddr_id, "capability": claim.capability},
                    )
                )
            elif not claim.established:
                spots.append(
                    BlindSpot(
                        kind=BlindSpotKind.UNTESTED,
                        subject=claim.name,
                        reason=(
                            f"only {claim.observations} observation(s); "
                            "confidence is not yet meaningful"
                        ),
                        severity=2,
                        context={"observations": claim.observations},
                    )
                )
            elif claim.last_used_ts and (now - claim.last_used_ts) > self.stale_after_s:
                age_days = (now - claim.last_used_ts) / 86400.0
                spots.append(
                    BlindSpot(
                        kind=BlindSpotKind.STALE,
                        subject=claim.name,
                        reason=f"no evidence for {age_days:.0f} days",
                        severity=2,
                        context={"age_days": round(age_days, 1)},
                    )
                )

        # Capabilities the lockbox grants that the self-model never claimed:
        # the system holds them and does not know it does.
        for ddr_id, token in self.self_model.lockbox._by_ddr.items():
            for cap in token.capability_mask:
                if (ddr_id, cap) not in claimed_caps:
                    spots.append(
                        BlindSpot(
                            kind=BlindSpotKind.UNCLAIMED,
                            subject=f"{ddr_id}:{cap}",
                            reason="granted by the lockbox but absent from the self-model",
                            severity=4,
                            context={"ddr": ddr_id, "capability": cap},
                        )
                    )

        spots.sort(key=lambda s: (-s.severity, s.subject))
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="blindspot.discover",
            gate_status="ok", found=len(spots),
            kinds=sorted({s.kind.value for s in spots}),
        )
        return spots

    def worst(self, limit: int = 5) -> list[BlindSpot]:
        return self.discover()[:limit]
