"""D-13 — failure memory registry.

The system's record of what has already been tried and did not work. It is the
real :class:`~aether.agents.research.hypothesis_generator.DeadEndLookup` that
D-07 pinned an interface for, and it closes the **I-06** wire: a hypothesis
matching a recorded dead end is blocked before any experiment cost is committed.

**Append-only, with no delete path at all.** That is the whole point and it is
enforced structurally rather than by policy — this module exposes no remove, no
clear, no overwrite. A registry that can forget is worse than no registry:
the single thing an agent under pressure most wants to do is drop the record of
the approach it is about to retry, and if that is possible the registry
guarantees exactly nothing. Superseding is available (:meth:`supersede`) and is
itself an append — the original entry stays readable forever.

Matching is on a normalised signature, so trivial rewording does not walk past a
dead end (the obvious evasion, and the one D-07's gate already pins).

Invariants: **S5** (append-only), **S9** (deterministic — string normalisation
and set membership, no model call), **S12** (nothing is silently lost; the store
is bounded only in what it keeps *in memory*, and eviction is audited while the
on-disk journal keeps everything).
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, Callable, Iterator

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "FailureKind",
    "FailureRecord",
    "FailureMemoryRegistry",
    "FailureRegistryError",
    "normalise_signature",
    "signature_divergence",
    "MIN_DIVERGENCE",
]

DDR_ID = "D-13"
_FILE = "aether/agents/memory/failure_memory_registry.py"
CAPABILITY = "CAP_FAILURE_REGISTRY"

DEFAULT_STORE = Path(__file__).resolve().parent / "failures.jsonl"


class FailureRegistryError(ValueError):
    """Registry operation refused."""


class FailureKind(str, Enum):
    DEAD_END = "dead_end"          # tried, falsified, do not retry
    ERROR = "error"                # crashed or errored out
    REJECTED = "rejected"          # refused by a gate or reviewer
    TIMEOUT = "timeout"


#: Below this divergence a new approach is too close to a recorded dead end to
#: be worth spending on (Section 3D #63). Conservative on purpose: a false
#: "too similar" costs one refusal the operator can override, while a false
#: "different enough" costs the full price of re-deriving a known failure.
MIN_DIVERGENCE = 0.30


def signature_divergence(a: str, b: str) -> float:
    """1.0 - Jaccard similarity over normalised tokens. 0.0 same, 1.0 disjoint."""
    ta = set(normalise_signature(a).split())
    tb = set(normalise_signature(b).split())
    if not ta and not tb:
        return 0.0
    if not ta or not tb:
        return 1.0
    return 1.0 - len(ta & tb) / len(ta | tb)


def normalise_signature(text: str) -> str:
    """Canonical form used for matching.

    Case and whitespace only — deliberately conservative. Aggressive
    normalisation (stemming, synonyms) would collapse genuinely different
    approaches onto one signature and block work that was never tried.
    """
    return " ".join(text.lower().split())


@dataclass(slots=True)
class FailureRecord:
    signature: str
    kind: FailureKind
    detail: str
    source_ddr: str = ""
    superseded_by: str = ""
    ts: float = field(default_factory=time.time)

    @property
    def active(self) -> bool:
        return not self.superseded_by

    def to_json(self) -> str:
        payload = asdict(self)
        payload["kind"] = self.kind.value
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))


class FailureMemoryRegistry:
    """Append-only memory of what has already failed.

    Satisfies the ``DeadEndLookup`` protocol D-07 declared, so it drops into
    ``HypothesisGenerator(dead_ends=...)`` with no change to D-07.
    """

    def __init__(
        self,
        lockbox: Lockbox,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
        alignment_tap: Callable[[str, dict[str, Any]], None] | None = None,
        replay: bool = True,
    ) -> None:
        self.lockbox = lockbox
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self.alignment_tap = alignment_tap
        self._records: list[FailureRecord] = []
        if replay:
            self._replay()

    def _emit(self, step: str, detail: dict[str, Any]) -> None:
        if self.alignment_tap is not None:
            self.alignment_tap(f"{DDR_ID}.{step}", detail)

    def _replay(self) -> None:
        """Rebuild from the journal. A dead end must survive a restart —
        otherwise every reboot is an amnesia window.

        Entries are applied **in journal order**, exactly as the runtime applies
        them: a plain entry adds an active record, and a supersession entry
        retires the records active at that point. Loading every line as an
        independent record instead would leave a superseded dead end still
        blocking, and would also mis-handle the real case where a signature is
        recorded again after being superseded (re-failed ⇒ active again).
        """
        if not self.store_path.is_file():
            return
        with self.store_path.open("r", encoding="utf-8") as fh:
            for line in fh:
                stripped = line.strip()
                if not stripped:
                    continue
                try:
                    rec = json.loads(stripped)
                except json.JSONDecodeError as exc:
                    raise FailureRegistryError(
                        f"corrupt failure registry: {self.store_path}"
                    ) from exc
                signature = rec["signature"]
                superseded_by = rec.get("superseded_by", "")
                if superseded_by:
                    for existing in self._records:
                        if existing.signature == signature and existing.active:
                            existing.superseded_by = superseded_by
                    continue
                self._records.append(FailureRecord(
                    signature=signature,
                    kind=FailureKind(rec.get("kind", "dead_end")),
                    detail=rec.get("detail", ""),
                    source_ddr=rec.get("source_ddr", ""),
                    ts=rec.get("ts", 0.0),
                ))

    def _journal(self, record: FailureRecord) -> None:
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(record.to_json() + "\n")

    def _append(self, record: FailureRecord) -> FailureRecord:
        self._records.append(record)
        self._journal(record)
        return record

    # -- writing -------------------------------------------------------------
    def record(
        self,
        signature: str,
        kind: FailureKind = FailureKind.DEAD_END,
        detail: str = "",
        source_ddr: str = "",
    ) -> FailureRecord:
        """Record a failure. There is no counterpart that removes one."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        norm = normalise_signature(signature)
        if not norm:
            raise FailureRegistryError("a failure needs a non-empty signature")
        if not detail.strip():
            # An unexplained dead end blocks future work while giving no way to
            # judge whether the block is still justified.
            raise FailureRegistryError(
                f"failure {norm!r} needs a detail explaining what went wrong"
            )
        rec = self._append(FailureRecord(
            signature=norm, kind=kind, detail=detail, source_ddr=source_ddr,
        ))
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="failure.record",
            gate_status=kind.value, signature=norm, source=source_ddr,
        )
        self._emit("record", {"signature": norm, "kind": kind.value})
        return rec

    def supersede(self, signature: str, reason: str) -> FailureRecord:
        """Mark a dead end no longer binding — by appending, never by deleting.

        Used when the world changed (a dependency landed, a limit was lifted).
        The original record stays readable, so "we un-blocked this and it failed
        again" remains visible in the history.
        """
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not reason.strip():
            raise FailureRegistryError("superseding requires a reason")
        norm = normalise_signature(signature)
        active = [r for r in self._records if r.signature == norm and r.active]
        if not active:
            raise FailureRegistryError(f"no active failure for {norm!r}")
        for rec in active:
            rec.superseded_by = reason
            # Journal-only: the retirement is a new line, while the in-memory
            # record is the same object the caller can still read. Appending it
            # to _records too would double-count the failure on every restart.
            self._journal(FailureRecord(
                signature=norm, kind=rec.kind, detail=rec.detail,
                source_ddr=rec.source_ddr, superseded_by=reason,
            ))
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="failure.superseded",
            gate_status="superseded", signature=norm, reason=reason,
        )
        self._emit("supersede", {"signature": norm})
        return active[0]

    # -- the DeadEndLookup protocol -----------------------------------------
    def is_dead_end(self, signature: str) -> bool:
        """D-07's I-06 pre-check. Only DEAD_END and un-superseded records block.

        An ERROR or TIMEOUT is not a dead end: the approach may be sound and the
        run merely unlucky, and blocking on those would freeze the system out of
        anything that ever crashed once.
        """
        norm = normalise_signature(signature)
        return any(r.signature == norm and r.active
                   and r.kind is FailureKind.DEAD_END
                   for r in self._records)

    # -- reading -------------------------------------------------------------
    def get(self, signature: str) -> list[FailureRecord]:
        norm = normalise_signature(signature)
        return [r for r in self._records if r.signature == norm]

    def dead_ends(self) -> list[str]:
        return sorted({r.signature for r in self._records
                       if r.active and r.kind is FailureKind.DEAD_END})

    def by_kind(self, kind: FailureKind) -> list[FailureRecord]:
        return [r for r in self._records if r.kind is kind]

    # -- divergence scoring (Section 3D #63, DDR-855) -------------------------
    def nearest_dead_end(
        self, text: str, threshold: float = MIN_DIVERGENCE
    ) -> tuple[FailureRecord, float] | None:
        """Closest recorded dead end to `text`, if it is nearer than `threshold`.

        `is_dead_end` matches an EXACT normalised signature, which catches
        re-proposing the same approach verbatim and nothing else. Section 3D #63
        asks for a divergence score so a *reworded* near-repeat is caught too —
        the obvious evasion once an agent learns exact matching exists.

        Returns the RECORD and its divergence, not a boolean, so a refusal can
        show the operator which dead end was hit and why that one failed. A bare
        `False` makes the refusal unarguable, and an unarguable refusal gets
        overridden on principle rather than on the merits.

        Deliberately NOT an embedding score. `1 - Jaccard` over normalised
        tokens is deterministic (S9) and explainable; "the model thought it was
        similar" is not a reason an operator can argue with, and this refusal
        must be arguable. It is also strictly a *supplement* — `is_dead_end`
        remains the exact-match gate D-07's I-06 wire calls.
        """
        if not text.strip():
            raise FailureRegistryError("cannot score an empty approach")
        if not 0.0 <= threshold <= 1.0:
            raise FailureRegistryError(f"threshold must be in [0,1], got {threshold}")

        best: FailureRecord | None = None
        best_d = 1.1
        for record in self._records:
            if not record.active or record.kind is not FailureKind.DEAD_END:
                continue
            d = signature_divergence(text, record.signature)
            if d < best_d:
                best, best_d = record, d
        if best is None or best_d >= threshold:
            return None
        return best, best_d

    @property
    def records(self) -> tuple[FailureRecord, ...]:
        return tuple(self._records)

    def __len__(self) -> int:
        return len(self._records)

    def __iter__(self) -> Iterator[FailureRecord]:
        return iter(tuple(self._records))
