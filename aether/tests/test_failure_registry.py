"""D-13 discriminating gate — failure memory registry, and the I-06 wire."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from aether.agents.memory.failure_memory_registry import (
    FailureKind,
    FailureMemoryRegistry,
    FailureRegistryError,
    normalise_signature,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _registry(tmp_path: Path, granted: bool = True, tap=None, store=None):
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("D-13", "failure_memory_registry.py", "CAP_FAILURE_REGISTRY")
    return FailureMemoryRegistry(
        lockbox=lb, store_path=store or (tmp_path / "failures.jsonl"),
        audit=audit, alignment_tap=tap,
    ), lb, audit


def test_registry_exposes_no_delete_path(tmp_path: Path) -> None:
    """THE discriminator — append-only enforced structurally, not by policy.

    The single thing an agent under pressure most wants is to drop the record
    of the approach it is about to retry. If any method can do that, the
    registry guarantees nothing. So there must be no such method at all.
    """
    reg, _lb, _audit = _registry(tmp_path)
    forbidden = {"delete", "remove", "clear", "pop", "discard", "drop",
                 "purge", "forget", "erase", "reset", "truncate", "update",
                 "overwrite", "set"}
    present = {n for n in dir(reg) if not n.startswith("_")} & forbidden
    assert present == set(), f"registry exposes a mutation path: {present}"


def test_recorded_dead_end_cannot_be_removed_only_superseded(tmp_path: Path) -> None:
    """Superseding is itself an append — the original stays readable, so
    'we un-blocked this and it failed again' remains visible."""
    reg, _lb, _audit = _registry(tmp_path)
    reg.record("bump the cache", detail="p99 got worse", source_ddr="D-10")
    assert reg.is_dead_end("bump the cache") is True

    reg.supersede("bump the cache", reason="allocator rewritten, worth retrying")
    assert reg.is_dead_end("bump the cache") is False
    # The record is still there, and still says what happened.
    history = reg.get("bump the cache")
    assert len(history) >= 1
    assert any("p99 got worse" in r.detail for r in history)


def test_i06_wire_blocks_regeneration_in_d07(tmp_path: Path) -> None:
    """The regression D-07 was written to make possible.

    D-07 pinned a DeadEndLookup interface and defaulted to a permissive stub.
    The real registry must drop in with NO change to D-07 and produce the same
    block the stub-based gate asserted.
    """
    from aether.agents.research.hypothesis_generator import (
        DeadEndBlocked,
        HypothesisGenerator,
    )

    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    lb.register("D-13", "failure_memory_registry.py", "CAP_FAILURE_REGISTRY")
    lb.register("D-07", "hypothesis_generator.py", "CAP_HYPOTHESIS_WRITE")

    reg = FailureMemoryRegistry(
        lockbox=lb, store_path=tmp_path / "failures.jsonl", audit=audit)
    reg.record("raising cache size reduces p99 latency",
               detail="falsified by D-10 run 7", source_ddr="D-10")

    def _gen(_c: str, _d: dict[str, Any]) -> list[dict[str, Any]]:
        return [{
            "statement": "Raising  CACHE size   reduces P99 latency",  # reworded
            "falsification_condition": "p99 does not fall",
            "expected_evidence": "p99 samples",
            "estimated_cost": 9.0,
        }]

    gen = HypothesisGenerator(
        lockbox=lb, generate_fn=_gen, dead_ends=reg,
        store_path=tmp_path / "hyp.jsonl", audit=audit,
    )
    with pytest.raises(DeadEndBlocked, match="recorded dead end"):
        gen.generate("cache sizing")
    assert gen.queued() == []
    assert not (tmp_path / "hyp.jsonl").exists()   # no cost committed


def test_unrecorded_hypothesis_still_generates(tmp_path: Path) -> None:
    """The registry must not block everything — that would pass the test above
    while making D-07 useless."""
    from aether.agents.research.hypothesis_generator import HypothesisGenerator

    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    lb.register("D-13", "f.py", "CAP_FAILURE_REGISTRY")
    lb.register("D-07", "h.py", "CAP_HYPOTHESIS_WRITE")
    reg = FailureMemoryRegistry(
        lockbox=lb, store_path=tmp_path / "failures.jsonl", audit=audit)
    reg.record("something else entirely", detail="nope")

    gen = HypothesisGenerator(
        lockbox=lb,
        generate_fn=lambda _c, _d: [{
            "statement": "a brand new idea", "falsification_condition": "f",
            "expected_evidence": "e", "estimated_cost": 1.0,
        }],
        dead_ends=reg, store_path=tmp_path / "hyp.jsonl", audit=audit,
    )
    assert len(gen.generate("ctx")) == 1


def test_dead_end_survives_a_restart(tmp_path: Path) -> None:
    """Otherwise every reboot is an amnesia window and the registry is theatre."""
    store = tmp_path / "failures.jsonl"
    reg, lb, audit = _registry(tmp_path, store=store)
    reg.record("try the greedy solver", detail="diverges on cycles")

    reloaded = FailureMemoryRegistry(lockbox=lb, store_path=store, audit=audit)
    assert reloaded.is_dead_end("try the greedy solver") is True


def test_supersession_survives_a_restart(tmp_path: Path) -> None:
    store = tmp_path / "failures.jsonl"
    reg, lb, audit = _registry(tmp_path, store=store)
    reg.record("try the greedy solver", detail="diverges on cycles")
    reg.supersede("try the greedy solver", reason="cycle detection added")

    reloaded = FailureMemoryRegistry(lockbox=lb, store_path=store, audit=audit)
    assert reloaded.is_dead_end("try the greedy solver") is False


def test_re_failure_after_supersession_blocks_again(tmp_path: Path) -> None:
    """"We un-blocked this and it failed again" must block again — including
    after a restart. Replaying journal lines as independent records gets this
    wrong in both directions."""
    store = tmp_path / "failures.jsonl"
    reg, lb, audit = _registry(tmp_path, store=store)
    reg.record("try the greedy solver", detail="diverges on cycles")
    reg.supersede("try the greedy solver", reason="cycle detection added")
    reg.record("try the greedy solver", detail="still diverges, now on DAGs")
    assert reg.is_dead_end("try the greedy solver") is True

    reloaded = FailureMemoryRegistry(lockbox=lb, store_path=store, audit=audit)
    assert reloaded.is_dead_end("try the greedy solver") is True


def test_only_dead_ends_block(tmp_path: Path) -> None:
    """An ERROR or TIMEOUT is not a dead end — blocking on those would freeze
    the system out of anything that ever crashed once."""
    reg, _lb, _audit = _registry(tmp_path)
    reg.record("flaky approach", kind=FailureKind.ERROR, detail="segfault once")
    reg.record("slow approach", kind=FailureKind.TIMEOUT, detail="took 10m")
    reg.record("bad approach", kind=FailureKind.DEAD_END, detail="disproved")
    assert reg.is_dead_end("flaky approach") is False
    assert reg.is_dead_end("slow approach") is False
    assert reg.is_dead_end("bad approach") is True


def test_matching_normalises_case_and_whitespace(tmp_path: Path) -> None:
    reg, _lb, _audit = _registry(tmp_path)
    reg.record("The  Greedy   Solver", detail="diverges")
    assert reg.is_dead_end("the greedy solver") is True
    assert reg.is_dead_end("  THE GREEDY    SOLVER  ") is True
    assert reg.is_dead_end("the greedy solver v2") is False


def test_normalisation_is_conservative() -> None:
    """Aggressive normalisation would collapse different approaches onto one
    signature and block work that was never tried."""
    assert normalise_signature("  A  B ") == "a b"
    assert normalise_signature("caching") != normalise_signature("cache")


def test_failure_without_detail_refused(tmp_path: Path) -> None:
    """An unexplained dead end blocks future work while giving no way to judge
    whether the block is still justified."""
    reg, _lb, _audit = _registry(tmp_path)
    with pytest.raises(FailureRegistryError, match="needs a detail"):
        reg.record("mystery", detail="   ")


def test_empty_signature_refused(tmp_path: Path) -> None:
    reg, _lb, _audit = _registry(tmp_path)
    with pytest.raises(FailureRegistryError, match="non-empty signature"):
        reg.record("   ", detail="x")


def test_supersede_requires_a_reason_and_an_active_record(tmp_path: Path) -> None:
    reg, _lb, _audit = _registry(tmp_path)
    reg.record("x", detail="d")
    with pytest.raises(FailureRegistryError, match="requires a reason"):
        reg.supersede("x", reason="  ")
    with pytest.raises(FailureRegistryError, match="no active failure"):
        reg.supersede("never recorded", reason="r")


def test_corrupt_journal_is_refused_not_silently_skipped(tmp_path: Path) -> None:
    """S12 — a half-readable registry silently under-blocks."""
    store = tmp_path / "failures.jsonl"
    reg, lb, audit = _registry(tmp_path, store=store)
    reg.record("x", detail="d")
    with store.open("a", encoding="utf-8") as fh:
        fh.write("{not json\n")
    with pytest.raises(FailureRegistryError, match="corrupt failure registry"):
        FailureMemoryRegistry(lockbox=lb, store_path=store, audit=audit)


def test_dead_ends_listing_and_queries(tmp_path: Path) -> None:
    reg, _lb, _audit = _registry(tmp_path)
    reg.record("b approach", detail="d")
    reg.record("a approach", detail="d")
    reg.record("errored", kind=FailureKind.ERROR, detail="d")
    assert reg.dead_ends() == ["a approach", "b approach"]
    assert len(reg.by_kind(FailureKind.ERROR)) == 1
    assert len(reg) == 3
    assert len(list(reg)) == 3


def test_recording_is_audited(tmp_path: Path) -> None:
    reg, _lb, _audit = _registry(tmp_path)
    reg.record("x", detail="d", source_ddr="D-10")
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "D-13" and e["action"] == "failure.record"
               for e in entries)


def test_capability_required(tmp_path: Path) -> None:
    reg, _lb, _audit = _registry(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_FAILURE_REGISTRY"):
        reg.record("x", detail="d")


def test_is_dead_end_needs_no_capability(tmp_path: Path) -> None:
    """D-07 calls this on every generation; requiring a capability there would
    make the I-06 pre-check fail open for any agent lacking the token."""
    reg, _lb, _audit = _registry(tmp_path, granted=False)
    assert reg.is_dead_end("anything") is False


def test_alignment_tap_receives_steps(tmp_path: Path) -> None:
    events: list[str] = []
    reg, _lb, _audit = _registry(tmp_path, tap=lambda s, _d: events.append(s))
    reg.record("x", detail="d")
    reg.supersede("x", reason="r")
    assert {"D-13.record", "D-13.supersede"} <= set(events)
