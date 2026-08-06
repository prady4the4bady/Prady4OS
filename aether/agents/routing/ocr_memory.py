"""OCR→memory pipeline and multi-modal context builder (3D #56/#57, DDR-852).

## #56 — OCR to memory

OCR is *lossy in a way that reads as authoritative*. A misrecognised digit does
not arrive flagged as uncertain; it arrives as a number, gets stored as a fact,
and is later retrieved with the same confidence as something the operator typed.
By the time it is wrong, nothing upstream records that it was ever a guess.

So the pipeline has one rule above the others: **confidence travels with the
text, and text below the floor is quarantined rather than stored.** Quarantined
means retrievable and clearly marked — not discarded (the operator may want it)
and not silently promoted (the failure above).

Provenance is mandatory for the same reason: a memory record whose source cannot
be named cannot be re-checked against the document when it is doubted.

## #57 — multi-modal context builder

Assembles text, OCR, memory and scene fragments into one context. Its job is to
make the *cost* of each fragment explicit before assembly, so the compressor has
real numbers to evict on.

**A fragment with no measurable token cost is refused.** Defaulting it to zero
would make unmeasured content free, and free content is never evicted — so the
one fragment nobody could size would survive every compression.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

from ..budget.compress import Segment, compress, estimate_tokens

__all__ = [
    "OcrError",
    "ContextError",
    "Modality",
    "OcrFragment",
    "MemoryRecord",
    "OcrPipeline",
    "ContextBuilder",
    "MIN_CONFIDENCE",
    "MAX_DOCUMENT_BYTES",
]

# Below this an OCR fragment is quarantined rather than stored as fact.
MIN_CONFIDENCE = 0.80

# §3C ACTION_PARSE_DOCUMENT caps documents at 64 MiB.
MAX_DOCUMENT_BYTES = 64 * 1024 * 1024


class OcrError(Exception):
    pass


class ContextError(Exception):
    pass


class Modality(Enum):
    TEXT = "text"
    OCR = "ocr"
    MEMORY = "memory"
    SCENE = "scene"


@dataclass(frozen=True)
class OcrFragment:
    """One recognised region, with the confidence that produced it."""

    text: str
    confidence: float
    page: int
    region: str = ""

    def __post_init__(self) -> None:
        if not 0.0 <= self.confidence <= 1.0:
            raise OcrError(f"confidence must be in [0,1], got {self.confidence}")
        if self.page < 0:
            raise OcrError("page must not be negative")


@dataclass(frozen=True)
class MemoryRecord:
    """A durable fact, with the provenance needed to re-check it."""

    text: str
    source_document: str
    page: int
    confidence: float
    quarantined: bool = False

    def __post_init__(self) -> None:
        if not self.source_document.strip():
            raise OcrError(
                "memory record requires a source document — a fact whose origin "
                "cannot be named cannot be re-checked when it is doubted"
            )


class OcrPipeline:
    """document → fragments → memory records, with confidence preserved."""

    def __init__(self, min_confidence: float = MIN_CONFIDENCE) -> None:
        if not 0.0 < min_confidence <= 1.0:
            raise OcrError("min_confidence must be in (0,1]")
        self.min_confidence = min_confidence
        self.stored: list[MemoryRecord] = []
        self.quarantined: list[MemoryRecord] = []

    def ingest(
        self, document: str, fragments: list[OcrFragment], size_bytes: int | None = None
    ) -> list[MemoryRecord]:
        """Convert fragments to memory records. Low confidence is quarantined.

        Returns everything produced, stored and quarantined alike, so a caller
        cannot mistake the quarantined set for "nothing was found there".
        """
        if not document.strip():
            raise OcrError("document must be named")
        n = len(document.encode("utf-8")) if size_bytes is None else size_bytes
        if size_bytes is not None and size_bytes > MAX_DOCUMENT_BYTES:
            raise OcrError(
                f"document is {size_bytes} bytes, over the {MAX_DOCUMENT_BYTES} "
                "cap (3C ACTION_PARSE_DOCUMENT)"
            )
        del n

        produced: list[MemoryRecord] = []
        seen: set[tuple[str, int]] = {
            (r.text, r.page) for r in self.stored + self.quarantined
        }
        for frag in fragments:
            if not frag.text.strip():
                continue
            key = (frag.text, frag.page)
            if key in seen:
                # Duplicate regions are common in multi-column OCR; storing them
                # twice makes a repeated line look like corroboration.
                continue
            seen.add(key)
            rec = MemoryRecord(
                text=frag.text,
                source_document=document,
                page=frag.page,
                confidence=frag.confidence,
                quarantined=frag.confidence < self.min_confidence,
            )
            (self.quarantined if rec.quarantined else self.stored).append(rec)
            produced.append(rec)
        return produced

    def facts(self) -> list[MemoryRecord]:
        """Only the records confident enough to be treated as fact."""
        return list(self.stored)


@dataclass
class ContextBuilder:
    """Assembles a multi-modal context and hands it to the compressor."""

    segments: list[Segment] = field(default_factory=list)
    _seq: int = 0

    def add(
        self,
        key: str,
        text: str,
        modality: Modality = Modality.TEXT,
        pinned: bool = False,
        priority: int | None = None,
    ) -> Segment:
        """Add one fragment. Its token cost is measured here, never assumed."""
        if not key.strip():
            raise ContextError("segment requires a key")
        if not isinstance(modality, Modality):
            raise ContextError(f"unknown modality {modality!r}")
        if not text.strip():
            raise ContextError(
                f"{key}: empty fragment. A fragment with no measurable token "
                "cost would be free, and free content is never evicted — the "
                "one thing nobody could size would survive every compression"
            )
        if any(s.key == key for s in self.segments):
            raise ContextError(f"duplicate segment key {key!r}")

        seg = Segment(
            key=key,
            text=text,
            pinned=pinned,
            priority=_DEFAULT_PRIORITY[modality] if priority is None else priority,
            seq=self._seq,
        )
        self._seq += 1
        self.segments.append(seg)
        return seg

    def add_memory(self, record: MemoryRecord, key: str) -> Segment:
        """Add a memory record, carrying its provenance INTO the context.

        A quarantined record enters marked and at the lowest priority, so it is
        evicted first and, if it survives, the model sees that it is uncertain.
        Passing it in clean would launder a low-confidence OCR guess into an
        authoritative-looking line of context.
        """
        marker = "[UNVERIFIED OCR] " if record.quarantined else ""
        return self.add(
            key,
            f"{marker}{record.text} "
            f"(source: {record.source_document} p{record.page}, "
            f"confidence {record.confidence:.2f})",
            modality=Modality.MEMORY,
            priority=-1 if record.quarantined else _DEFAULT_PRIORITY[Modality.MEMORY],
        )

    @property
    def tokens(self) -> int:
        return sum(estimate_tokens(s.text) for s in self.segments)

    def build(self, target_fraction: float = 0.8):
        """Compress to the target. Propagates `CompressionImpossible`."""
        if not self.segments:
            raise ContextError("cannot build an empty context")
        return compress(self.segments, target_fraction)


# Higher survives compression longer. Ordering rationale: the task text and the
# operator's own words outrank recalled memory, which outranks OCR, which
# outranks a scene description that will be re-derivable next frame.
_DEFAULT_PRIORITY = {
    Modality.TEXT: 5,
    Modality.MEMORY: 3,
    Modality.OCR: 2,
    Modality.SCENE: 1,
}
