"""D-11 — knowledge consolidation.

Merges what the system has learned from many sources into one coherent store,
and — the part that matters — **resolves conflicts instead of accumulating
them.**

A memory that holds "the cache is write-through" and "the cache is write-back"
simultaneously is worse than a memory holding neither, because every consumer
gets whichever it happens to read first and no consumer can tell it was a coin
flip. So consolidation is total: for each claim key exactly one belief survives,
chosen by a fixed precedence, and the loser is **retained as superseded with the
reason**, never dropped.

Retention is not sentiment. A silently discarded belief (**S12**) makes the
reversal invisible: when the winner later turns out to be wrong, nothing records
that the system already held the right answer and threw it away. The superseded
trail is what makes that recoverable.

Precedence, highest first — the ordering is fixed so consolidation is
reproducible (**S9**):

1. **evidence tier** — measured beats derived beats asserted;
2. **corroboration** — more independent sources;
3. **confidence**;
4. **recency**;
5. **source name**, purely to break exact ties deterministically.

Recency sits *fourth*, deliberately. A newer assertion from a weaker source
overriding a measurement is how a well-evidenced fact gets talked out of the
store by repetition.
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, Callable, Iterable

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "EvidenceTier",
    "Belief",
    "Consolidation",
    "KnowledgeConsolidator",
    "ConsolidationError",
    "MAX_BELIEFS",
]

DDR_ID = "D-11"
_FILE = "aether/agents/memory/knowledge_consolidation.py"
CAPABILITY = "CAP_MEMORY_WRITE"

DEFAULT_STORE = Path(__file__).resolve().parent / "knowledge.jsonl"
MAX_BELIEFS = 10_000            # S2: bounded store


class ConsolidationError(ValueError):
    """Consolidation refused."""


class EvidenceTier(int, Enum):
    """How the belief was arrived at. Higher wins."""

    ASSERTED = 1                # someone said so
    DERIVED = 2                 # inferred from other beliefs
    MEASURED = 3                # observed directly


@dataclass(slots=True)
class Belief:
    """One claim about the world, with its provenance."""

    key: str
    value: str
    source: str
    tier: EvidenceTier = EvidenceTier.ASSERTED
    confidence: float = 0.5
    corroborations: int = 1
    ts: float = field(default_factory=time.time)
    superseded_by: str = ""
    superseded_reason: str = ""

    @property
    def active(self) -> bool:
        return not self.superseded_by

    def rank(self) -> tuple[int, int, float, float, str]:
        """The precedence key. Recency is fourth, on purpose."""
        return (int(self.tier), self.corroborations, self.confidence,
                self.ts, self.source)

    def to_json(self) -> str:
        payload = asdict(self)
        payload["tier"] = int(self.tier)
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))


@dataclass(slots=True)
class Consolidation:
    """The result for one conflicted key."""

    key: str
    winner: Belief
    superseded: list[Belief]
    reason: str


class KnowledgeConsolidator:
    """Consolidates beliefs into one active belief per key."""

    def __init__(
        self,
        lockbox: Lockbox,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
        alignment_tap: Callable[[str, dict[str, Any]], None] | None = None,
        max_beliefs: int = MAX_BELIEFS,
    ) -> None:
        self.lockbox = lockbox
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self.alignment_tap = alignment_tap
        self.max_beliefs = int(max_beliefs)
        self._beliefs: list[Belief] = []

    def _emit(self, step: str, detail: dict[str, Any]) -> None:
        if self.alignment_tap is not None:
            self.alignment_tap(f"{DDR_ID}.{step}", detail)

    # -- ingestion -----------------------------------------------------------
    def add(self, belief: Belief) -> Belief:
        """Record a belief. Bounded (S2); an eviction is audited (S12)."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not belief.key:
            raise ConsolidationError("a belief needs a key")
        if not belief.source:
            raise ConsolidationError(f"belief {belief.key!r} needs a source")
        if not 0.0 <= belief.confidence <= 1.0:
            raise ConsolidationError(
                f"confidence {belief.confidence} outside [0, 1]"
            )
        if belief.corroborations < 1:
            raise ConsolidationError("corroborations must be >= 1")
        if len(self._beliefs) >= self.max_beliefs:
            dropped = self._beliefs.pop(0)
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="knowledge.belief_evicted",
                gate_status="dropped", reason="belief store full",
                key=dropped.key, source=dropped.source, window=self.max_beliefs,
            )
        self._beliefs.append(belief)
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(belief.to_json() + "\n")
        self._emit("add", {"key": belief.key, "source": belief.source})
        return belief

    def add_many(self, beliefs: Iterable[Belief]) -> int:
        return sum(1 for b in beliefs if self.add(b))

    # -- consolidation -------------------------------------------------------
    def consolidate(self) -> list[Consolidation]:
        """Resolve every conflicted key. Total by construction."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        by_key: dict[str, list[Belief]] = {}
        for belief in self._beliefs:
            if belief.active:
                by_key.setdefault(belief.key, []).append(belief)

        results: list[Consolidation] = []
        for key in sorted(by_key):
            candidates = by_key[key]
            values = {b.value for b in candidates}
            if len(values) <= 1:
                continue                    # agreement is not a conflict
            winner = max(candidates, key=lambda b: b.rank())
            reason = (f"tier={winner.tier.name} corroborations="
                      f"{winner.corroborations} confidence={winner.confidence}")
            losers: list[Belief] = []
            for belief in candidates:
                if belief is winner:
                    continue
                # S12: retained, not deleted. A silently dropped belief makes
                # the reversal invisible if the winner is later disproved.
                belief.superseded_by = winner.source
                belief.superseded_reason = reason
                losers.append(belief)
                with self.store_path.open("a", encoding="utf-8") as fh:
                    fh.write(belief.to_json() + "\n")
                self.audit.append_action(
                    ddr=DDR_ID, file=_FILE, action="knowledge.superseded",
                    gate_status="superseded", key=key, loser=belief.source,
                    winner=winner.source, reason=reason,
                )
            results.append(Consolidation(key=key, winner=winner,
                                         superseded=losers, reason=reason))
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="knowledge.consolidate",
                gate_status="resolved", key=key, winner=winner.source,
                superseded=len(losers),
            )
        self._emit("consolidate", {"conflicts": len(results)})
        return results

    # -- reads ---------------------------------------------------------------
    def get(self, key: str) -> Belief | None:
        """The one active belief for ``key`` after consolidation."""
        active = [b for b in self._beliefs if b.key == key and b.active]
        if not active:
            return None
        return max(active, key=lambda b: b.rank())

    def active_beliefs(self) -> list[Belief]:
        return sorted((b for b in self._beliefs if b.active),
                      key=lambda b: (b.key, b.source))

    def superseded_beliefs(self) -> list[Belief]:
        return sorted((b for b in self._beliefs if not b.active),
                      key=lambda b: (b.key, b.source))

    def history(self, key: str) -> list[Belief]:
        """Everything ever believed about ``key``, winners and losers."""
        return sorted((b for b in self._beliefs if b.key == key),
                      key=lambda b: b.ts)

    @property
    def beliefs(self) -> tuple[Belief, ...]:
        return tuple(self._beliefs)
