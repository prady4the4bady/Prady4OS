"""D-07 — hypothesis generator.

Turns causal gaps (D-06) and world-model state (D-04) into hypotheses that can
actually be run as experiments.

The rule that does the work: **a hypothesis without a falsification condition is
not a hypothesis.** Every generated item must carry

    statement · falsification_condition · expected_evidence · estimated_cost

and any missing or blank field is a refusal. An untestable hypothesis is exactly
what must never reach the experiment queue — it consumes budget and can never
return a negative result, so it can only ever confirm.

**I-06 (dead-end pre-check).** A hypothesis matching a known dead-end is blocked
*before* generation completes, not after the experiment burns cost. The narrow
interface is :class:`DeadEndLookup`; **D-13**
(``aether.agents.memory.failure_memory_registry.FailureMemoryRegistry``) is the
real implementation and drops in unchanged. :data:`NO_DEAD_ENDS` remains the
default so a generator constructed without a registry is permissive rather than
silently blocking everything.
"""

from __future__ import annotations

import json
import time
import uuid
from dataclasses import asdict, dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, Callable, Protocol

from aether.agents.causal.causal_reasoning_engine import CausalLink, LinkKind
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "Hypothesis",
    "HypothesisStatus",
    "HypothesisGenerator",
    "HypothesisError",
    "DeadEndBlocked",
    "DeadEndLookup",
    "NO_DEAD_ENDS",
    "REQUIRED_FIELDS",
]

DDR_ID = "D-07"
_FILE = "aether/agents/research/hypothesis_generator.py"
CAPABILITY = "CAP_HYPOTHESIS_WRITE"

DEFAULT_STORE = Path(__file__).resolve().parent / "hypotheses.jsonl"
MAX_PER_RUN = 16                      # S2: bounded generation

REQUIRED_FIELDS = (
    "statement",
    "falsification_condition",
    "expected_evidence",
    "estimated_cost",
)


class HypothesisError(ValueError):
    """Hypothesis refused as malformed."""


class DeadEndBlocked(RuntimeError):
    """The hypothesis matches a recorded dead end (I-06)."""


class HypothesisStatus(str, Enum):
    PROPOSED = "proposed"
    QUEUED = "queued"
    CONFIRMED = "confirmed"
    FALSIFIED = "falsified"


class DeadEndLookup(Protocol):
    """The interface D-13's FailureMemoryRegistry satisfies."""

    def is_dead_end(self, signature: str) -> bool: ...


class _NoDeadEnds:
    """Permissive default: used when no registry is wired in."""

    def is_dead_end(self, signature: str) -> bool:      # noqa: D102
        return False


NO_DEAD_ENDS: DeadEndLookup = _NoDeadEnds()


@dataclass(slots=True)
class Hypothesis:
    hyp_id: str
    statement: str
    falsification_condition: str
    expected_evidence: str
    estimated_cost: float
    origin: str = ""                  # the causal link / gap that motivated it
    status: HypothesisStatus = HypothesisStatus.PROPOSED
    ts: float = field(default_factory=time.time)

    @property
    def signature(self) -> str:
        """Stable identity used for dead-end matching.

        Normalised so trivial rewording does not slip past the registry — the
        obvious way an agent would otherwise re-propose a known dead end.
        """
        return " ".join(self.statement.lower().split())

    def to_json(self) -> str:
        payload = asdict(self)
        payload["status"] = self.status.value
        payload["signature"] = self.signature
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))


#: Produces raw hypothesis dicts from a prompt/context. Injected (S9).
GenerateFn = Callable[[str, dict[str, Any]], list[dict[str, Any]]]


class HypothesisGenerator:
    """Generates falsifiable hypotheses, pre-checked against dead ends."""

    def __init__(
        self,
        lockbox: Lockbox,
        generate_fn: GenerateFn,
        dead_ends: DeadEndLookup | None = None,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
        alignment_tap: Callable[[str, dict[str, Any]], None] | None = None,
        max_per_run: int = MAX_PER_RUN,
    ) -> None:
        self.lockbox = lockbox
        self.generate_fn = generate_fn
        self.dead_ends: DeadEndLookup = dead_ends if dead_ends is not None else NO_DEAD_ENDS
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self.alignment_tap = alignment_tap
        self.max_per_run = int(max_per_run)
        self._hypotheses: dict[str, Hypothesis] = {}

    def _emit(self, step: str, detail: dict[str, Any]) -> None:
        if self.alignment_tap is not None:
            self.alignment_tap(f"{DDR_ID}.{step}", detail)

    # -- validation ----------------------------------------------------------
    @staticmethod
    def _validate(raw: Any) -> dict[str, Any]:
        if not isinstance(raw, dict):
            raise HypothesisError(
                f"hypothesis must be an object, got {type(raw).__name__}"
            )
        missing = [f for f in REQUIRED_FIELDS if f not in raw]
        if missing:
            raise HypothesisError(f"hypothesis missing fields: {missing}")
        for field_name in ("statement", "falsification_condition", "expected_evidence"):
            value = raw[field_name]
            if not isinstance(value, str) or not value.strip():
                raise HypothesisError(f"{field_name} must be a non-empty string")
        cost = raw["estimated_cost"]
        if isinstance(cost, bool) or not isinstance(cost, (int, float)):
            raise HypothesisError(
                f"estimated_cost must be a number, got {type(cost).__name__}"
            )
        if cost < 0:
            raise HypothesisError(f"estimated_cost {cost} must be >= 0")
        return dict(raw)

    # -- generation ----------------------------------------------------------
    def generate(self, context: str, detail: dict[str, Any] | None = None) -> list[Hypothesis]:
        """Generate hypotheses for ``context``, refusing untestable ones."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not context:
            raise HypothesisError("generation needs a context")
        try:
            raw_items = self.generate_fn(context, dict(detail or {}))
        except Exception as exc:
            raise HypothesisError(
                f"generation failed: {type(exc).__name__}: {exc}"
            ) from exc
        if not isinstance(raw_items, list):
            raise HypothesisError(
                f"generate_fn must return a list, got {type(raw_items).__name__}"
            )
        if len(raw_items) > self.max_per_run:
            raise HypothesisError(
                f"{len(raw_items)} hypotheses exceeds the per-run cap {self.max_per_run}"
            )

        out: list[Hypothesis] = []
        for raw in raw_items:
            fields = self._validate(raw)
            hyp = Hypothesis(
                hyp_id=str(uuid.uuid4()),
                statement=str(fields["statement"]),
                falsification_condition=str(fields["falsification_condition"]),
                expected_evidence=str(fields["expected_evidence"]),
                estimated_cost=float(fields["estimated_cost"]),
                origin=str(fields.get("origin", context)),
            )
            # I-06: blocked BEFORE the hypothesis is stored or queued, so no
            # experiment cost is ever committed to a known dead end.
            if self.dead_ends.is_dead_end(hyp.signature):
                self.audit.append_action(
                    ddr=DDR_ID, file=_FILE, action="hypothesis.dead_end_blocked",
                    gate_status="blocked", signature=hyp.signature,
                )
                self._emit("dead_end_blocked", {"signature": hyp.signature})
                raise DeadEndBlocked(
                    f"hypothesis matches a recorded dead end: {hyp.signature!r}"
                )
            self._hypotheses[hyp.hyp_id] = hyp
            with self.store_path.open("a", encoding="utf-8") as fh:
                fh.write(hyp.to_json() + "\n")
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="hypothesis.generate",
                gate_status="proposed", hyp_id=hyp.hyp_id, cost=hyp.estimated_cost,
            )
            self._emit("generate", {"hyp_id": hyp.hyp_id})
            out.append(hyp)
        return out

    def from_causal_gap(self, link: CausalLink) -> str:
        """Turn an unresolved causal relation into a generation context.

        CONFOUNDED and CORRELATED links are where knowledge is missing; a CAUSAL
        link is already settled and generating for it wastes budget.
        """
        if link.kind is LinkKind.CAUSAL:
            raise HypothesisError(
                f"{link.cause}->{link.effect} is already CAUSAL; no gap to probe"
            )
        if link.kind is LinkKind.CONFOUNDED:
            return (
                f"does {link.cause} affect {link.effect} independently of "
                f"{link.confounder}?"
            )
        return f"is the association between {link.cause} and {link.effect} causal?"

    # -- lifecycle -----------------------------------------------------------
    def get(self, hyp_id: str) -> Hypothesis | None:
        return self._hypotheses.get(hyp_id)

    def queued(self) -> list[Hypothesis]:
        return sorted(
            (h for h in self._hypotheses.values()
             if h.status in (HypothesisStatus.PROPOSED, HypothesisStatus.QUEUED)),
            key=lambda h: (h.estimated_cost, h.ts),
        )

    def set_status(self, hyp_id: str, status: HypothesisStatus) -> Hypothesis:
        hyp = self._hypotheses.get(hyp_id)
        if hyp is None:
            raise HypothesisError(f"unknown hypothesis: {hyp_id}")
        hyp.status = status
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(hyp.to_json() + "\n")
        self._emit("status", {"hyp_id": hyp_id, "status": status.value})
        return hyp
