"""D-07 discriminating gate — hypothesis generator."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from aether.agents.causal.causal_reasoning_engine import CausalLink, LinkKind
from aether.agents.research.hypothesis_generator import (
    NO_DEAD_ENDS,
    DeadEndBlocked,
    Hypothesis,
    HypothesisError,
    HypothesisGenerator,
    HypothesisStatus,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _item(**over: Any) -> dict[str, Any]:
    item: dict[str, Any] = {
        "statement": "raising cache size reduces p99 latency",
        "falsification_condition": "p99 latency does not fall by >=5% at 2x cache",
        "expected_evidence": "p99 latency samples before and after",
        "estimated_cost": 4.0,
    }
    item.update(over)
    return item


class _DeadEnds:
    def __init__(self, *signatures: str) -> None:
        self.signatures = {s.lower() for s in signatures}
        self.queried: list[str] = []

    def is_dead_end(self, signature: str) -> bool:
        self.queried.append(signature)
        return signature in self.signatures


def _gen(
    tmp_path: Path, items=None, dead_ends=None, granted: bool = True, tap=None
) -> HypothesisGenerator:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("D-07", "hypothesis_generator.py", "CAP_HYPOTHESIS_WRITE")
    return HypothesisGenerator(
        lockbox=lb,
        generate_fn=lambda _c, _d: (items if items is not None else [_item()]),
        dead_ends=dead_ends,
        store_path=tmp_path / "hypotheses.jsonl", audit=audit, alignment_tap=tap,
    )


def test_generates_a_falsifiable_hypothesis(tmp_path: Path) -> None:
    hyps = _gen(tmp_path).generate("cache sizing")
    assert len(hyps) == 1
    h = hyps[0]
    assert h.falsification_condition
    assert h.expected_evidence
    assert h.estimated_cost == 4.0
    assert h.status is HypothesisStatus.PROPOSED


@pytest.mark.parametrize("missing", [
    "statement", "falsification_condition", "expected_evidence", "estimated_cost",
])
def test_missing_required_field_is_refused(tmp_path: Path, missing: str) -> None:
    """An untestable hypothesis can only ever confirm — it must not be queued."""
    bad = _item()
    del bad[missing]
    with pytest.raises(HypothesisError, match="missing fields"):
        _gen(tmp_path, items=[bad]).generate("x")


def test_blank_falsification_condition_is_refused(tmp_path: Path) -> None:
    """Present-but-empty is the same defect as absent."""
    with pytest.raises(HypothesisError, match="falsification_condition"):
        _gen(tmp_path, items=[_item(falsification_condition="   ")]).generate("x")


def test_dead_end_is_blocked_before_the_experiment(tmp_path: Path) -> None:
    """I-06 — the discriminator.

    Blocking AFTER generation still lets the cost be committed. The hypothesis
    must not be stored, must not be queued, and the registry must have been
    consulted.
    """
    known = "raising cache size reduces p99 latency"
    dead = _DeadEnds(known)
    gen = _gen(tmp_path, dead_ends=dead)
    with pytest.raises(DeadEndBlocked, match="recorded dead end"):
        gen.generate("cache sizing")
    assert dead.queried == [known]
    assert gen.queued() == []
    assert not (tmp_path / "hypotheses.jsonl").exists()


def test_dead_end_matching_survives_rewording(tmp_path: Path) -> None:
    """Whitespace/case rewording is the obvious way to slip past the registry."""
    dead = _DeadEnds("raising cache size reduces p99 latency")
    reworded = _item(statement="  RAISING   Cache Size   REDUCES P99 Latency  ")
    with pytest.raises(DeadEndBlocked):
        _gen(tmp_path, items=[reworded], dead_ends=dead).generate("x")


def test_default_lookup_blocks_nothing(tmp_path: Path) -> None:
    """A generator with no registry wired in must be permissive, not silently
    blocking everything. D-13 supplies the real lookup."""
    assert NO_DEAD_ENDS.is_dead_end("anything at all") is False
    gen = _gen(tmp_path)
    assert gen.dead_ends is NO_DEAD_ENDS
    assert len(gen.generate("x")) == 1


def test_per_run_cap_enforced(tmp_path: Path) -> None:
    """S2 — bounded generation."""
    many = [_item(statement=f"hypothesis {i}") for i in range(40)]
    with pytest.raises(HypothesisError, match="exceeds the per-run cap"):
        _gen(tmp_path, items=many).generate("x")


def test_negative_cost_refused(tmp_path: Path) -> None:
    with pytest.raises(HypothesisError, match="must be >= 0"):
        _gen(tmp_path, items=[_item(estimated_cost=-1.0)]).generate("x")


def test_bool_cost_refused(tmp_path: Path) -> None:
    """True == 1 would pass a naive numeric check."""
    with pytest.raises(HypothesisError, match="estimated_cost must be a number"):
        _gen(tmp_path, items=[_item(estimated_cost=True)]).generate("x")


def test_non_object_and_non_list_refused(tmp_path: Path) -> None:
    with pytest.raises(HypothesisError, match="must be an object"):
        _gen(tmp_path, items=["not an object"]).generate("x")
    with pytest.raises(HypothesisError, match="must return a list"):
        _gen(tmp_path, items={"not": "a list"}).generate("x")


def test_generation_failure_is_wrapped(tmp_path: Path) -> None:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    lb.register("D-07", "h.py", "CAP_HYPOTHESIS_WRITE")

    def explode(_c: str, _d: dict[str, Any]) -> list[dict[str, Any]]:
        raise RuntimeError("model offline")

    gen = HypothesisGenerator(
        lockbox=lb, generate_fn=explode,
        store_path=tmp_path / "h.jsonl", audit=audit,
    )
    with pytest.raises(HypothesisError, match="generation failed"):
        gen.generate("x")


def test_causal_gap_contexts(tmp_path: Path) -> None:
    """Gaps come from CONFOUNDED/CORRELATED links; a settled link is not a gap."""
    gen = _gen(tmp_path)
    confounded = CausalLink("ice_cream", "drownings", LinkKind.CONFOUNDED,
                            confounder="temperature")
    assert "temperature" in gen.from_causal_gap(confounded)
    correlated = CausalLink("a", "b", LinkKind.CORRELATED)
    assert "causal" in gen.from_causal_gap(correlated)
    settled = CausalLink("switch", "lamp", LinkKind.CAUSAL)
    with pytest.raises(HypothesisError, match="already CAUSAL"):
        gen.from_causal_gap(settled)


def test_queue_is_cost_ordered(tmp_path: Path) -> None:
    items = [
        _item(statement="expensive one", estimated_cost=90.0),
        _item(statement="cheap one", estimated_cost=1.0),
    ]
    gen = _gen(tmp_path, items=items)
    gen.generate("x")
    assert [h.statement for h in gen.queued()] == ["cheap one", "expensive one"]


def test_status_transitions(tmp_path: Path) -> None:
    gen = _gen(tmp_path)
    hyp = gen.generate("x")[0]
    gen.set_status(hyp.hyp_id, HypothesisStatus.FALSIFIED)
    assert gen.get(hyp.hyp_id).status is HypothesisStatus.FALSIFIED  # type: ignore[union-attr]
    assert gen.queued() == []
    with pytest.raises(HypothesisError, match="unknown hypothesis"):
        gen.set_status("nope", HypothesisStatus.CONFIRMED)


def test_capability_required(tmp_path: Path) -> None:
    with pytest.raises(CapabilityError, match="CAP_HYPOTHESIS_WRITE"):
        _gen(tmp_path, granted=False).generate("x")


def test_alignment_tap_receives_steps(tmp_path: Path) -> None:
    events: list[str] = []
    gen = _gen(tmp_path, tap=lambda step, _d: events.append(step))
    hyp = gen.generate("x")[0]
    gen.set_status(hyp.hyp_id, HypothesisStatus.QUEUED)
    assert "D-07.generate" in events
    assert "D-07.status" in events


def test_signature_normalisation() -> None:
    a = Hypothesis("1", "  Cache   SIZE matters ", "f", "e", 1.0)
    b = Hypothesis("2", "cache size matters", "f", "e", 1.0)
    assert a.signature == b.signature


def test_generation_is_audited(tmp_path: Path) -> None:
    _gen(tmp_path).generate("x")
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "D-07" and e["action"] == "hypothesis.generate"
               for e in entries)
