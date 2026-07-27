"""C-10 — cross-domain analogy engine.

Surfaces structural analogies between concepts in *different* domains — "neural
attention" ↔ "immune clonal selection" — to support the creative-insight
dimension.

The rule that makes it an analogy engine rather than a synonym finder:
**candidates must come from a different domain than the source.** A same-domain
match is a paraphrase, and paraphrases will always dominate a similarity ranking
because surface vocabulary overlaps. Filtering them out is what forces the engine
to reach.

Second rule: **structural similarity is scored separately from retrieval
similarity.** Retrieval finds plausible pairs by embedding proximity; a scorer
then judges whether the *structure* corresponds. Ranking on retrieval distance
alone returns whatever shares words, which is exactly the failure mode a
cross-domain engine exists to avoid.

Both retrieval and scoring are injected, so gates are deterministic.
"""

from __future__ import annotations

import json
import math
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Callable, Sequence

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "Concept",
    "AnalogyCandidate",
    "AnalogyEngine",
    "AnalogyError",
    "cosine",
]

DDR_ID = "C-10"
_FILE = "aether/agents/analogy/cross_domain_engine.py"
CAPABILITY = "CAP_ANALOGY_READ"

DEFAULT_LOG = Path(__file__).resolve().parent / "analogy_log.jsonl"


class AnalogyError(RuntimeError):
    """Analogy search refused."""


@dataclass(slots=True, frozen=True)
class Concept:
    name: str
    domain: str
    embedding: tuple[float, ...] = ()


@dataclass(slots=True)
class AnalogyCandidate:
    source_concept: str
    source_domain: str
    target_concept: str
    target_domain: str
    structural_similarity: float
    explanation: str = ""
    retrieval_similarity: float = 0.0

    def to_json(self) -> str:
        payload = asdict(self)
        payload["ts"] = time.time()
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def cosine(a: Sequence[float], b: Sequence[float]) -> float:
    """Cosine similarity, 0.0 for a zero or mismatched vector."""
    if not a or not b or len(a) != len(b):
        return 0.0
    dot = sum(x * y for x, y in zip(a, b))
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(y * y for y in b))
    if na == 0.0 or nb == 0.0:
        return 0.0
    return dot / (na * nb)


#: Judges structural correspondence and explains it, independent of retrieval.
StructureScoreFn = Callable[[Concept, Concept], tuple[float, str]]


class AnalogyEngine:
    """Finds cross-domain structural analogies."""

    def __init__(
        self,
        lockbox: Lockbox,
        concepts: Sequence[Concept] = (),
        score_fn: StructureScoreFn | None = None,
        log_path: str | Path | None = None,
        audit: AuditLog | None = None,
    ) -> None:
        self.lockbox = lockbox
        self._concepts: list[Concept] = list(concepts)
        self.score_fn = score_fn
        self.log_path = Path(log_path) if log_path is not None else DEFAULT_LOG
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()

    # -- knowledge -----------------------------------------------------------
    def add_concept(self, concept: Concept) -> Concept:
        self._concepts.append(concept)
        return concept

    @property
    def concepts(self) -> tuple[Concept, ...]:
        return tuple(self._concepts)

    def domains(self) -> tuple[str, ...]:
        return tuple(sorted({c.domain for c in self._concepts}))

    # -- search --------------------------------------------------------------
    def find_analogies(
        self,
        concept: Concept,
        target_domains: Sequence[str] | None = None,
        top_k: int = 5,
        min_structural: float = 0.0,
    ) -> list[AnalogyCandidate]:
        """Top-k cross-domain analogies for ``concept``."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        if top_k < 1:
            raise AnalogyError("top_k must be >= 1")
        if not concept.name or not concept.domain:
            raise AnalogyError("a source concept needs a name and a domain")

        wanted = set(target_domains) if target_domains else None
        if wanted is not None and concept.domain in wanted:
            raise AnalogyError(
                f"target_domains must exclude the source domain {concept.domain!r}"
            )

        pool = [
            c
            for c in self._concepts
            # A same-domain match is a paraphrase, not an analogy — and it would
            # win on surface vocabulary every time.
            if c.domain != concept.domain
            and (wanted is None or c.domain in wanted)
            and c.name != concept.name
        ]
        if not pool:
            return []

        # Retrieval proposes; structure disposes.
        retrieved = sorted(
            ((cosine(concept.embedding, c.embedding), c) for c in pool),
            key=lambda pair: (-pair[0], pair[1].name),
        )

        candidates: list[AnalogyCandidate] = []
        for retrieval_sim, target in retrieved:
            if self.score_fn is None:
                structural, explanation = retrieval_sim, "retrieval similarity only"
            else:
                try:
                    structural, explanation = self.score_fn(concept, target)
                except Exception as exc:
                    self.audit.append_action(
                        ddr=DDR_ID, file=_FILE, action="analogy.score_error",
                        gate_status="error", target=target.name,
                        error=f"{type(exc).__name__}: {exc}",
                    )
                    continue
            if structural < min_structural:
                continue
            candidates.append(
                AnalogyCandidate(
                    source_concept=concept.name,
                    source_domain=concept.domain,
                    target_concept=target.name,
                    target_domain=target.domain,
                    structural_similarity=round(float(structural), 4),
                    explanation=explanation,
                    retrieval_similarity=round(float(retrieval_sim), 4),
                )
            )

        # Ranked by STRUCTURE, not by retrieval distance.
        candidates.sort(
            key=lambda c: (-c.structural_similarity, c.target_concept)
        )
        top = candidates[:top_k]

        with self.log_path.open("a", encoding="utf-8") as fh:
            for candidate in top:
                fh.write(candidate.to_json() + "\n")
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="analogy.search", gate_status="ok",
            source=concept.name, source_domain=concept.domain,
            found=len(top),
        )
        return top
