"""C-10 discriminating gate — cross-domain analogy engine."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.agents.analogy.cross_domain_engine import (
    AnalogyEngine,
    AnalogyError,
    Concept,
    cosine,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox

ATTENTION = Concept("neural attention", "machine_learning", (1.0, 0.0, 0.0))
# Same domain, nearly identical vector: a paraphrase that would win on distance.
SELF_ATTENTION = Concept("self attention", "machine_learning", (0.99, 0.01, 0.0))
CLONAL = Concept("clonal selection", "immunology", (0.8, 0.6, 0.0))
MARKET = Concept("price discovery", "economics", (0.0, 1.0, 0.0))


def _engine(
    tmp_path: Path,
    concepts=(SELF_ATTENTION, CLONAL, MARKET),
    score_fn=None,
    granted: bool = True,
) -> AnalogyEngine:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("C-10", "cross_domain_engine.py", "CAP_ANALOGY_READ")
    return AnalogyEngine(
        lockbox=lb, concepts=concepts, score_fn=score_fn,
        log_path=tmp_path / "analogy_log.jsonl", audit=audit,
    )


def test_finds_top_analogy(tmp_path: Path) -> None:
    """The mandated gate: at least one candidate with similarity > 0."""
    engine = _engine(tmp_path)
    results = engine.find_analogies(ATTENTION, top_k=5)
    assert len(results) >= 1
    assert results[0].structural_similarity > 0.0
    assert results[0].source_concept == "neural attention"


def test_same_domain_paraphrase_is_excluded(tmp_path: Path) -> None:
    """The discriminator.

    'self attention' sits in the SOURCE domain with a near-identical vector, so
    it wins any pure similarity ranking. An engine that does not filter by
    domain returns a paraphrase and calls it an analogy — which is the one thing
    a cross-domain engine must not do.
    """
    engine = _engine(tmp_path)
    results = engine.find_analogies(ATTENTION, top_k=5)
    assert all(r.target_domain != "machine_learning" for r in results)
    assert all(r.target_concept != "self attention" for r in results)


def test_structure_outranks_retrieval(tmp_path: Path) -> None:
    """Retrieval proposes, structure disposes.

    'price discovery' is the FURTHEST by embedding but scores highest
    structurally. Ranking by retrieval distance would bury it.
    """

    def scorer(_src: Concept, target: Concept) -> tuple[float, str]:
        scores = {"price discovery": 0.95, "clonal selection": 0.2}
        return scores.get(target.name, 0.0), f"structural read of {target.name}"

    engine = _engine(tmp_path, score_fn=scorer)
    results = engine.find_analogies(ATTENTION, top_k=5)
    assert results[0].target_concept == "price discovery"
    # And it really was the worse retrieval match.
    assert results[0].retrieval_similarity < results[1].retrieval_similarity
    assert "structural read" in results[0].explanation


def test_target_domain_filter(tmp_path: Path) -> None:
    engine = _engine(tmp_path)
    results = engine.find_analogies(ATTENTION, target_domains=["immunology"], top_k=5)
    assert {r.target_domain for r in results} == {"immunology"}


def test_source_domain_cannot_be_a_target(tmp_path: Path) -> None:
    engine = _engine(tmp_path)
    with pytest.raises(AnalogyError, match="must exclude the source domain"):
        engine.find_analogies(ATTENTION, target_domains=["machine_learning"])


def test_min_structural_filters(tmp_path: Path) -> None:
    def scorer(_src: Concept, target: Concept) -> tuple[float, str]:
        return (0.9, "strong") if target.name == "clonal selection" else (0.1, "weak")

    engine = _engine(tmp_path, score_fn=scorer)
    results = engine.find_analogies(ATTENTION, top_k=5, min_structural=0.5)
    assert [r.target_concept for r in results] == ["clonal selection"]


def test_top_k_is_respected(tmp_path: Path) -> None:
    engine = _engine(tmp_path)
    assert len(engine.find_analogies(ATTENTION, top_k=1)) == 1


def test_empty_pool_returns_nothing(tmp_path: Path) -> None:
    engine = _engine(tmp_path, concepts=(SELF_ATTENTION,))   # same domain only
    assert engine.find_analogies(ATTENTION) == []


def test_a_failing_scorer_skips_that_candidate(tmp_path: Path) -> None:
    """One bad score must not lose the whole search."""

    def flaky(_src: Concept, target: Concept) -> tuple[float, str]:
        if target.name == "clonal selection":
            raise RuntimeError("scorer blew up")
        return 0.7, "fine"

    engine = _engine(tmp_path, score_fn=flaky)
    results = engine.find_analogies(ATTENTION, top_k=5)
    assert [r.target_concept for r in results] == ["price discovery"]


def test_capability_required(tmp_path: Path) -> None:
    engine = _engine(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_ANALOGY_READ"):
        engine.find_analogies(ATTENTION)


def test_input_validation(tmp_path: Path) -> None:
    engine = _engine(tmp_path)
    with pytest.raises(AnalogyError, match="top_k"):
        engine.find_analogies(ATTENTION, top_k=0)
    with pytest.raises(AnalogyError, match="name and a domain"):
        engine.find_analogies(Concept("", "x"))


@pytest.mark.parametrize(
    ("a", "b", "expected"),
    [
        ((1.0, 0.0), (1.0, 0.0), 1.0),
        ((1.0, 0.0), (0.0, 1.0), 0.0),
        ((0.0, 0.0), (1.0, 0.0), 0.0),      # zero vector, no NaN
        ((1.0,), (1.0, 0.0), 0.0),          # mismatched length
        ((), (), 0.0),
    ],
)
def test_cosine(a: tuple[float, ...], b: tuple[float, ...], expected: float) -> None:
    assert cosine(a, b) == pytest.approx(expected)


def test_results_logged_and_audited(tmp_path: Path) -> None:
    engine = _engine(tmp_path)
    engine.find_analogies(ATTENTION, top_k=2)
    lines = (tmp_path / "analogy_log.jsonl").read_text(encoding="utf-8").splitlines()
    assert len(lines) >= 1
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "C-10" and e["action"] == "analogy.search" for e in entries)
