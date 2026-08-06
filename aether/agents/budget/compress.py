"""TokenJuice context compression (Section 3D #50, DDR-850).

The spec is specific: compress a context to **≤80% of its original token count**.

The obvious implementation truncates until it fits. That is the failure this
module is built to avoid, and it is the same one the budget in `__init__.py`
avoids: an agent handed a silently shortened context produces a confident answer
from less information than the caller believes it had.

So compression here has one hard rule and one refusal:

- **PINNED SEGMENTS ARE NEVER DROPPED.** The system prompt, the skill body, the
  current task, the safety invariants. These are the segments whose absence
  changes what the agent *is*, not merely what it knows.
- **If the target cannot be met without dropping a pinned segment, compress()
  RAISES.** It does not return a best-effort context, because a best-effort
  context is indistinguishable from a successful one at the call site.

Eviction order is by explicit priority then by age, and it is deterministic:
the same context compresses the same way every time. A compressor whose output
depends on dict ordering or wall-clock makes a bad answer impossible to
reproduce, and an irreproducible bad answer gets retried rather than fixed.
"""

from __future__ import annotations

from dataclasses import dataclass, field

__all__ = [
    "CompressionImpossible",
    "Segment",
    "CompressionResult",
    "compress",
    "estimate_tokens",
    "TARGET_FRACTION",
]

# Section 3D #50: the compressed context must be at most 80% of the original.
TARGET_FRACTION = 0.80


class CompressionImpossible(Exception):
    """The target cannot be met without dropping protected content."""


def estimate_tokens(text: str) -> int:
    """Words / 0.75 — the same estimator used everywhere else in the tree.

    One estimator, project-wide. Two would let a context pass the compressor
    and fail the budget, and the disagreement would read as a flaky bug rather
    than a definition problem.
    """
    return int(len(text.split()) / 0.75)


@dataclass(frozen=True)
class Segment:
    """One addressable piece of context.

    `pinned` is not a hint. A pinned segment is one whose removal changes what
    the agent is rather than what it knows, and no target justifies dropping it.
    """

    key: str
    text: str
    pinned: bool = False
    priority: int = 0  # higher survives longer among unpinned segments
    seq: int = 0       # arrival order; ties break toward keeping the NEWER

    @property
    def tokens(self) -> int:
        return estimate_tokens(self.text)


@dataclass
class CompressionResult:
    """What survived, what was evicted, and the numbers behind both."""

    segments: list[Segment]
    dropped: list[Segment] = field(default_factory=list)
    original_tokens: int = 0
    final_tokens: int = 0

    @property
    def fraction(self) -> float:
        if self.original_tokens == 0:
            return 0.0
        return self.final_tokens / self.original_tokens

    def render(self, separator: str = "\n\n") -> str:
        return separator.join(s.text for s in self.segments)


def compress(
    segments: list[Segment], target_fraction: float = TARGET_FRACTION
) -> CompressionResult:
    """Reduce to at most `target_fraction` of the original token count.

    Raises `CompressionImpossible` if the pinned segments alone exceed the
    target. Returning a context that overshoots would be worse than raising:
    the caller would proceed believing the budget held.
    """
    if not 0.0 < target_fraction <= 1.0:
        raise ValueError(f"target_fraction must be in (0,1], got {target_fraction}")
    if not segments:
        return CompressionResult([], [], 0, 0)

    keys = [s.key for s in segments]
    if len(set(keys)) != len(keys):
        dupes = sorted({k for k in keys if keys.count(k) > 1})
        raise ValueError(
            f"duplicate segment keys {dupes} — a context with two segments "
            "under one key cannot be reasoned about, and eviction would drop "
            "an unpredictable one of them"
        )

    original = sum(s.tokens for s in segments)
    budget = int(original * target_fraction)

    pinned = [s for s in segments if s.pinned]
    pinned_tokens = sum(s.tokens for s in pinned)
    if pinned_tokens > budget:
        raise CompressionImpossible(
            f"pinned segments are {pinned_tokens} tokens, over the "
            f"{budget}-token target ({target_fraction:.0%} of {original}). "
            "Refusing to return a best-effort context: at the call site it is "
            "indistinguishable from a successful one, and the agent proceeds "
            "believing it has information it does not have"
        )

    # Evict the least valuable first: lowest priority, then oldest. Fully
    # deterministic — the same context compresses the same way every time, so a
    # bad answer can be reproduced and fixed rather than retried.
    unpinned = sorted(
        (s for s in segments if not s.pinned), key=lambda s: (s.priority, s.seq)
    )

    kept_unpinned: list[Segment] = []
    dropped: list[Segment] = []
    running = pinned_tokens
    # Walk from most valuable down, keeping while there is room.
    for seg in reversed(unpinned):
        if running + seg.tokens <= budget:
            kept_unpinned.append(seg)
            running += seg.tokens
        else:
            dropped.append(seg)

    kept = sorted(pinned + kept_unpinned, key=lambda s: s.seq)
    return CompressionResult(
        kept, sorted(dropped, key=lambda s: s.seq), original, running
    )
