"""D-14 discriminating gate — lineage knowledge loader."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.agents.memory.lineage_knowledge_loader import (
    MAX_ANCESTRY_DEPTH,
    LineageEntry,
    LineageError,
    LineageKnowledgeLoader,
    LineageNotLoaded,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _loader(tmp_path: Path, granted: bool = True, tap=None, generation="gen-2"):
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("D-14", "lineage_knowledge_loader.py", "CAP_ETHICS_DELIBERATE")
    return LineageKnowledgeLoader(
        lockbox=lb, generation=generation, store_path=tmp_path / "lineage.jsonl",
        audit=audit, alignment_tap=tap,
    )


def _entries(gen: str) -> list[LineageEntry]:
    return [
        LineageEntry(gen, "dead_end", "greedy solver", "diverges on cycles"),
        LineageEntry(gen, "belief", "cache.mode", "write-through"),
    ]


def _answer(question: str, inherited: list[LineageEntry]) -> str:
    return f"{question}|{len(inherited)}"


def test_inference_before_load_is_refused(tmp_path: Path) -> None:
    """THE discriminator — S13 as a hard precondition.

    A system that answers before its lineage is in cannot detect the state it
    is in: it re-derives settled facts and re-proposes recorded dead ends, and
    then records those re-derivations as fresh evidence. The corruption is not
    the wasted calls — it is that generation two's memory now holds confident
    conclusions reached in ignorance of generation one, unmarked.
    """
    ldr = _loader(tmp_path)
    assert ldr.loaded is False
    with pytest.raises(LineageNotLoaded, match="before its lineage was loaded"):
        ldr.infer("what do we know?", _answer)


def test_no_lazy_load_on_first_use(tmp_path: Path) -> None:
    """A lazy load would hide the very defect this exists to prevent: the first
    caller gets a correct answer and the ordering bug only shows under
    concurrency. A refused call must leave the loader unloaded."""
    ldr = _loader(tmp_path)
    with pytest.raises(LineageNotLoaded):
        ldr.infer("q", _answer)
    assert ldr.loaded is False
    assert len(ldr) == 0
    with pytest.raises(LineageNotLoaded):        # still refused, not primed
        ldr.infer("q", _answer)


def test_inference_works_after_load(tmp_path: Path) -> None:
    """The guard must not be uselessly strict."""
    ldr = _loader(tmp_path)
    ldr.load(["gen-0", "gen-1"],
             {"gen-0": lambda: _entries("gen-0"), "gen-1": lambda: _entries("gen-1")})
    assert ldr.loaded is True
    assert ldr.infer("q", _answer) == "q|4"


def test_premature_inference_is_audited(tmp_path: Path) -> None:
    ldr = _loader(tmp_path)
    with pytest.raises(LineageNotLoaded):
        ldr.infer("secret question", _answer)
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "D-14"
               and e["action"] == "lineage.premature_inference"
               and e["gate_status"] == "refused" for e in entries)


def test_ancestry_is_loaded_oldest_first(tmp_path: Path) -> None:
    """Order matters: a newer generation's belief must be able to supersede an
    older one downstream, which only works if the older arrives first."""
    ldr = _loader(tmp_path)
    got = ldr.load(["gen-0", "gen-1"],
                   {"gen-0": lambda: _entries("gen-0"),
                    "gen-1": lambda: _entries("gen-1")})
    assert [e.generation for e in got] == ["gen-0", "gen-0", "gen-1", "gen-1"]
    assert ldr.ancestry == ("gen-0", "gen-1")


def test_missing_ancestor_source_is_refused(tmp_path: Path) -> None:
    """A hole in the inheritance must not pass silently — proceeding would start
    a generation that believes it loaded everything."""
    ldr = _loader(tmp_path)
    with pytest.raises(LineageError, match="no source for ancestor"):
        ldr.load(["gen-0", "gen-1"], {"gen-0": lambda: _entries("gen-0")})
    assert ldr.loaded is False


def test_failed_ancestor_load_leaves_loader_unloaded(tmp_path: Path) -> None:
    """A partial inheritance must not count as loaded."""
    def boom() -> list[LineageEntry]:
        raise RuntimeError("store unreadable")

    ldr = _loader(tmp_path)
    with pytest.raises(LineageError, match="failed"):
        ldr.load(["gen-0"], {"gen-0": boom})
    assert ldr.loaded is False
    with pytest.raises(LineageNotLoaded):
        ldr.infer("q", _answer)


def test_cycle_in_ancestry_refused(tmp_path: Path) -> None:
    """A cycle re-inherits forever, and the duplicates would read as
    independent corroboration in D-11."""
    ldr = _loader(tmp_path)
    with pytest.raises(LineageError, match="cycle in ancestry"):
        ldr.load(["gen-0", "gen-1", "gen-0"], {})


def test_self_ancestry_refused(tmp_path: Path) -> None:
    ldr = _loader(tmp_path, generation="gen-2")
    with pytest.raises(LineageError, match="lists itself as an ancestor"):
        ldr.load(["gen-0", "gen-2"], {})


def test_ancestry_depth_bounded(tmp_path: Path) -> None:
    """S2."""
    ldr = _loader(tmp_path)
    deep = [f"gen-{i}" for i in range(MAX_ANCESTRY_DEPTH + 5)]
    with pytest.raises(LineageError, match="exceeds the cap"):
        ldr.load(deep, {})


def test_empty_ancestry_loads_cleanly(tmp_path: Path) -> None:
    """Generation zero has nothing to inherit but is still 'loaded' — it must
    not be blocked forever."""
    ldr = _loader(tmp_path, generation="gen-0")
    assert ldr.load([], {}) == []
    assert ldr.loaded is True
    assert ldr.infer("q", _answer) == "q|0"


def test_inherited_dead_ends_are_queryable(tmp_path: Path) -> None:
    """This is what feeds D-13/D-07 so generation two does not re-propose what
    generation one already disproved."""
    ldr = _loader(tmp_path)
    ldr.load(["gen-0"], {"gen-0": lambda: _entries("gen-0")})
    assert ldr.dead_ends() == ["greedy solver"]
    assert len(ldr.inherited("belief")) == 1
    assert len(ldr.inherited()) == 2


def test_load_is_persisted_and_audited(tmp_path: Path) -> None:
    ldr = _loader(tmp_path)
    ldr.load(["gen-0"], {"gen-0": lambda: _entries("gen-0")})
    assert (tmp_path / "lineage.jsonl").read_text(encoding="utf-8").strip()
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    rec = [e for e in entries if e["action"] == "lineage.load"]
    assert rec and rec[0]["extra"]["entries"] == 2


def test_capability_required(tmp_path: Path) -> None:
    ldr = _loader(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_ETHICS_DELIBERATE"):
        ldr.load([], {})


def test_alignment_tap_receives_steps(tmp_path: Path) -> None:
    events: list[str] = []
    ldr = _loader(tmp_path, tap=lambda s, _d: events.append(s))
    with pytest.raises(LineageNotLoaded):
        ldr.infer("q", _answer)
    ldr.load(["gen-0"], {"gen-0": lambda: _entries("gen-0")})
    ldr.infer("q", _answer)
    assert {"D-14.premature_inference", "D-14.load", "D-14.infer"} <= set(events)
