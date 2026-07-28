"""D-14 — lineage knowledge loader.

Loads what previous generations of the system learned — dead ends (D-13),
consolidated beliefs (D-11), recorded limits (B-15) — and makes that inheritance
a **precondition of inference rather than a background task**.

The ordering is the whole feature, and **S13** is why. A system that starts
answering before its lineage is loaded spends its first N calls in a state it
cannot detect: it does not know what it already knows, so it re-derives settled
facts, re-proposes recorded dead ends, and — worst — records those re-derivations
as fresh evidence. The corruption is not the wasted calls; it is that the second
generation's memory now contains confident conclusions reached in ignorance of
the first, and nothing marks them as such.

So :meth:`infer` refuses to run until :meth:`load` has completed. Not a warning,
not a lazy load-on-first-use — a hard :class:`LineageNotLoaded`. A lazy load
would paper over exactly the bug this exists to prevent, because the first
caller would silently get a correct answer and the ordering defect would only
appear under concurrency.

**S2**: a lineage chain is bounded; a cycle in the ancestry is refused rather
than followed.
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "LineageEntry",
    "LineageKnowledgeLoader",
    "LineageNotLoaded",
    "LineageError",
    "MAX_ANCESTRY_DEPTH",
]

DDR_ID = "D-14"
_FILE = "aether/agents/memory/lineage_knowledge_loader.py"
CAPABILITY = "CAP_ETHICS_DELIBERATE"

DEFAULT_STORE = Path(__file__).resolve().parent / "lineage.jsonl"
MAX_ANCESTRY_DEPTH = 32          # S2: bounded chain


class LineageError(ValueError):
    """Lineage operation refused."""


class LineageNotLoaded(RuntimeError):
    """Inference was attempted before the lineage finished loading (S13)."""


@dataclass(slots=True)
class LineageEntry:
    """One inherited item, tagged with the generation it came from."""

    generation: str
    kind: str                    # "dead_end" | "belief" | "limit"
    key: str
    value: str
    ts: float = field(default_factory=time.time)

    def to_json(self) -> str:
        return json.dumps(
            {"generation": self.generation, "kind": self.kind, "key": self.key,
             "value": self.value, "ts": self.ts},
            sort_keys=True, separators=(",", ":"),
        )


class LineageKnowledgeLoader:
    """Inherits prior generations' knowledge before any inference may run."""

    def __init__(
        self,
        lockbox: Lockbox,
        generation: str,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
        alignment_tap: Callable[[str, dict[str, Any]], None] | None = None,
        max_depth: int = MAX_ANCESTRY_DEPTH,
    ) -> None:
        self.lockbox = lockbox
        self.generation = generation
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self.alignment_tap = alignment_tap
        self.max_depth = int(max_depth)
        self._loaded = False
        self._entries: list[LineageEntry] = []
        self._ancestry: list[str] = []

    def _emit(self, step: str, detail: dict[str, Any]) -> None:
        if self.alignment_tap is not None:
            self.alignment_tap(f"{DDR_ID}.{step}", detail)

    @property
    def loaded(self) -> bool:
        return self._loaded

    # -- loading -------------------------------------------------------------
    def load(
        self,
        ancestry: list[str],
        sources: dict[str, Callable[[], list[LineageEntry]]] | None = None,
    ) -> list[LineageEntry]:
        """Walk the ancestry oldest-first and inherit each generation's records.

        ``sources`` maps a generation id to a callable returning its entries
        (injected, so this is deterministic and testable — S9).
        """
        self.lockbox.require(DDR_ID, CAPABILITY)
        if len(ancestry) > self.max_depth:
            raise LineageError(
                f"ancestry depth {len(ancestry)} exceeds the cap {self.max_depth}"
            )
        seen: set[str] = set()
        for gen in ancestry:
            if gen in seen:
                # A cycle would otherwise re-inherit forever, and the duplicate
                # entries would read as independent corroboration in D-11.
                raise LineageError(f"cycle in ancestry at {gen!r}")
            seen.add(gen)
        if self.generation in seen:
            raise LineageError(
                f"generation {self.generation!r} lists itself as an ancestor"
            )

        loaders = sources or {}
        inherited: list[LineageEntry] = []
        for gen in ancestry:                      # oldest first
            fetch = loaders.get(gen)
            if fetch is None:
                # A missing ancestor is a hole in the inheritance, not a
                # detail to shrug at: proceeding would silently start a
                # generation that believes it loaded everything.
                raise LineageError(f"no source for ancestor generation {gen!r}")
            try:
                entries = fetch()
            except Exception as exc:
                raise LineageError(
                    f"loading generation {gen!r} failed: "
                    f"{type(exc).__name__}: {exc}"
                ) from exc
            for entry in entries:
                inherited.append(entry)
                with self.store_path.open("a", encoding="utf-8") as fh:
                    fh.write(entry.to_json() + "\n")

        self._entries = inherited
        self._ancestry = list(ancestry)
        self._loaded = True
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="lineage.load", gate_status="loaded",
            generation=self.generation, ancestors=len(ancestry),
            entries=len(inherited),
        )
        self._emit("load", {"generation": self.generation,
                            "entries": len(inherited)})
        return inherited

    # -- the guarded operation ----------------------------------------------
    def infer(self, question: str, answer_fn: Callable[[str, list[LineageEntry]], str]) -> str:
        """Answer ``question`` — but only once the lineage is in.

        The refusal is hard on purpose. Loading lazily here would hide the
        ordering defect: the first caller would get a correct answer and the
        bug would surface only under concurrency, in production.
        """
        if not self._loaded:
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="lineage.premature_inference",
                gate_status="refused", generation=self.generation,
                question=question[:120],
            )
            self._emit("premature_inference", {"generation": self.generation})
            raise LineageNotLoaded(
                f"generation {self.generation!r} attempted inference before its "
                f"lineage was loaded; call load() first"
            )
        answer = answer_fn(question, list(self._entries))
        self._emit("infer", {"generation": self.generation})
        return answer

    # -- reads ---------------------------------------------------------------
    def inherited(self, kind: str | None = None) -> list[LineageEntry]:
        if kind is None:
            return list(self._entries)
        return [e for e in self._entries if e.kind == kind]

    def dead_ends(self) -> list[str]:
        return sorted({e.key for e in self._entries if e.kind == "dead_end"})

    @property
    def ancestry(self) -> tuple[str, ...]:
        return tuple(self._ancestry)

    def __len__(self) -> int:
        return len(self._entries)
