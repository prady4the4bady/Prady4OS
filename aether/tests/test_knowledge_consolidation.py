"""D-11 discriminating gate — knowledge consolidation."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.agents.memory.knowledge_consolidation import (
    Belief,
    ConsolidationError,
    EvidenceTier,
    KnowledgeConsolidator,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _consolidator(tmp_path: Path, granted: bool = True, tap=None, max_beliefs=10_000):
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("D-11", "knowledge_consolidation.py", "CAP_MEMORY_WRITE")
    return KnowledgeConsolidator(
        lockbox=lb, store_path=tmp_path / "knowledge.jsonl", audit=audit,
        alignment_tap=tap, max_beliefs=max_beliefs,
    )


def test_conflict_yields_exactly_one_winner_and_a_logged_loser(tmp_path: Path) -> None:
    """THE discriminator.

    A memory holding "write-through" and "write-back" at once is worse than one
    holding neither: every consumer gets whichever it reads first and none can
    tell it was a coin flip. Exactly one belief survives per key, and the loser
    is retained as superseded WITH a reason — deleting it makes the reversal
    invisible if the winner is later disproved.
    """
    kc = _consolidator(tmp_path)
    kc.add(Belief("cache.mode", "write-back", "guesser",
                  EvidenceTier.ASSERTED, confidence=0.9))
    kc.add(Belief("cache.mode", "write-through", "benchmark",
                  EvidenceTier.MEASURED, confidence=0.7))

    results = kc.consolidate()
    assert len(results) == 1
    result = results[0]
    assert result.winner.source == "benchmark"
    assert result.winner.value == "write-through"

    # Exactly one active belief remains for the key.
    active = [b for b in kc.active_beliefs() if b.key == "cache.mode"]
    assert len(active) == 1
    assert kc.get("cache.mode").value == "write-through"      # type: ignore[union-attr]

    # The loser is retained, not dropped, and says why it lost.
    assert len(result.superseded) == 1
    loser = result.superseded[0]
    assert loser.source == "guesser"
    assert loser.superseded_by == "benchmark"
    assert "MEASURED" in loser.superseded_reason
    assert loser in kc.superseded_beliefs()
    assert len(kc.history("cache.mode")) == 2                 # both survive


def test_measurement_beats_a_newer_higher_confidence_assertion(tmp_path: Path) -> None:
    """Recency is fourth in the precedence, deliberately.

    Ranking by recency first lets a well-evidenced fact be talked out of the
    store by repetition from a weaker source.
    """
    kc = _consolidator(tmp_path)
    measured = Belief("disk.fs", "sfs", "mkfs-probe", EvidenceTier.MEASURED,
                      confidence=0.6, ts=100.0)
    asserted = Belief("disk.fs", "ext4", "rumour", EvidenceTier.ASSERTED,
                      confidence=1.0, ts=999_999.0)
    kc.add(measured)
    kc.add(asserted)
    assert kc.consolidate()[0].winner.source == "mkfs-probe"


def test_corroboration_breaks_a_same_tier_tie(tmp_path: Path) -> None:
    kc = _consolidator(tmp_path)
    kc.add(Belief("k", "a", "one", EvidenceTier.DERIVED, 0.9, corroborations=1))
    kc.add(Belief("k", "b", "many", EvidenceTier.DERIVED, 0.5, corroborations=4))
    assert kc.consolidate()[0].winner.source == "many"


def test_confidence_then_recency_break_remaining_ties(tmp_path: Path) -> None:
    kc = _consolidator(tmp_path)
    kc.add(Belief("k", "a", "lo", EvidenceTier.DERIVED, 0.4, 2, ts=500.0))
    kc.add(Belief("k", "b", "hi", EvidenceTier.DERIVED, 0.8, 2, ts=100.0))
    assert kc.consolidate()[0].winner.source == "hi"

    kc2 = _consolidator(tmp_path / "b")
    kc2.add(Belief("k", "a", "old", EvidenceTier.DERIVED, 0.5, 2, ts=100.0))
    kc2.add(Belief("k", "b", "new", EvidenceTier.DERIVED, 0.5, 2, ts=900.0))
    assert kc2.consolidate()[0].winner.source == "new"


def test_agreement_is_not_a_conflict(tmp_path: Path) -> None:
    """Two sources saying the same thing must not supersede either."""
    kc = _consolidator(tmp_path)
    kc.add(Belief("k", "same", "a", EvidenceTier.MEASURED))
    kc.add(Belief("k", "same", "b", EvidenceTier.ASSERTED))
    assert kc.consolidate() == []
    assert kc.superseded_beliefs() == []
    assert len(kc.active_beliefs()) == 2


def test_consolidation_is_total_across_keys(tmp_path: Path) -> None:
    """Every conflicted key is resolved, not just the first found."""
    kc = _consolidator(tmp_path)
    for key in ("a", "b", "c"):
        kc.add(Belief(key, "x", "weak", EvidenceTier.ASSERTED))
        kc.add(Belief(key, "y", "strong", EvidenceTier.MEASURED))
    results = kc.consolidate()
    assert [r.key for r in results] == ["a", "b", "c"]
    assert all(r.winner.source == "strong" for r in results)
    assert len(kc.superseded_beliefs()) == 3


def test_consolidate_is_idempotent(tmp_path: Path) -> None:
    """Re-running must not re-supersede an already resolved key."""
    kc = _consolidator(tmp_path)
    kc.add(Belief("k", "a", "weak", EvidenceTier.ASSERTED))
    kc.add(Belief("k", "b", "strong", EvidenceTier.MEASURED))
    assert len(kc.consolidate()) == 1
    assert kc.consolidate() == []
    assert len(kc.superseded_beliefs()) == 1


def test_consolidation_is_deterministic(tmp_path: Path) -> None:
    """S9 — insertion order must not change the winner."""
    beliefs = [
        Belief("k", "a", "s1", EvidenceTier.DERIVED, 0.5, 2, ts=10.0),
        Belief("k", "b", "s2", EvidenceTier.DERIVED, 0.5, 2, ts=10.0),
        Belief("k", "c", "s3", EvidenceTier.DERIVED, 0.5, 2, ts=10.0),
    ]
    def _copy(b: Belief) -> Belief:
        return Belief(key=b.key, value=b.value, source=b.source, tier=b.tier,
                      confidence=b.confidence, corroborations=b.corroborations,
                      ts=b.ts)

    forward = _consolidator(tmp_path / "f")
    for b in beliefs:
        forward.add(_copy(b))
    reverse = _consolidator(tmp_path / "r")
    for b in reversed(beliefs):
        reverse.add(_copy(b))
    assert forward.consolidate()[0].winner.source == \
        reverse.consolidate()[0].winner.source


def test_supersession_is_audited(tmp_path: Path) -> None:
    kc = _consolidator(tmp_path)
    kc.add(Belief("k", "a", "weak", EvidenceTier.ASSERTED))
    kc.add(Belief("k", "b", "strong", EvidenceTier.MEASURED))
    kc.consolidate()
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    sup = [e for e in entries if e["action"] == "knowledge.superseded"]
    assert len(sup) == 1
    assert sup[0]["extra"]["loser"] == "weak"
    assert sup[0]["extra"]["winner"] == "strong"
    assert sup[0]["extra"]["reason"]


def test_store_is_bounded_and_evictions_logged(tmp_path: Path) -> None:
    """S2 bounded + S12 no silent loss."""
    kc = _consolidator(tmp_path, max_beliefs=3)
    for i in range(6):
        kc.add(Belief(f"k{i}", "v", f"s{i}", EvidenceTier.ASSERTED))
    assert len(kc.beliefs) == 3
    drops = [e for e in AuditLog(tmp_path / "audit_log.jsonl").read_all()
             if e["action"] == "knowledge.belief_evicted"]
    assert len(drops) == 3
    assert drops[0]["extra"]["reason"]


def test_get_returns_none_for_unknown_key(tmp_path: Path) -> None:
    assert _consolidator(tmp_path).get("nope") is None


@pytest.mark.parametrize(
    ("belief", "match"),
    [
        (Belief("", "v", "s"), "needs a key"),
        (Belief("k", "v", ""), "needs a source"),
        (Belief("k", "v", "s", confidence=1.5), r"outside \[0, 1\]"),
        (Belief("k", "v", "s", corroborations=0), "must be >= 1"),
    ],
)
def test_malformed_beliefs_refused(tmp_path: Path, belief, match) -> None:
    with pytest.raises(ConsolidationError, match=match):
        _consolidator(tmp_path).add(belief)


def test_capability_required(tmp_path: Path) -> None:
    kc = _consolidator(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_MEMORY_WRITE"):
        kc.add(Belief("k", "v", "s"))


def test_alignment_tap_receives_steps(tmp_path: Path) -> None:
    events: list[str] = []
    kc = _consolidator(tmp_path, tap=lambda s, _d: events.append(s))
    kc.add(Belief("k", "v", "s"))
    kc.consolidate()
    assert {"D-11.add", "D-11.consolidate"} <= set(events)


def test_tier_ordering() -> None:
    assert EvidenceTier.MEASURED > EvidenceTier.DERIVED > EvidenceTier.ASSERTED
