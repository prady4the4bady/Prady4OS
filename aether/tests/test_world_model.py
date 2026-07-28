"""D-04 discriminating gate — persistent world model."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.agents.world_model.world_model import (
    Entity,
    Relation,
    WorldModel,
    WorldModelError,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox

WRITER = "D-04"
READER = "C-10"


def _model(tmp_path: Path) -> WorldModel:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    lb.register(WRITER, "world_model.py", "CAP_WORLD_MODEL_WRITE")
    lb.register(READER, "cross_domain_engine.py", "CAP_ANALOGY_READ")
    return WorldModel(
        lockbox=lb,
        entities_path=tmp_path / "entities.jsonl",
        relations_path=tmp_path / "relations.jsonl",
        audit=audit,
    )


def test_add_and_query(tmp_path: Path) -> None:
    """The mandated gate: entity + 2 relations, query, restart, same result."""
    wm = _model(tmp_path)
    wm.add_entity(Entity("e1", "attention", "concept"), agent_ddr=WRITER)
    wm.add_entity(Entity("e2", "transformer", "architecture"), agent_ddr=WRITER)
    wm.add_entity(Entity("e3", "softmax", "operation"), agent_ddr=WRITER)
    wm.add_relation(Relation("r1", "e1", "e2", "part_of"), agent_ddr=WRITER)
    wm.add_relation(Relation("r2", "e1", "e3", "uses"), agent_ddr=WRITER)

    result = wm.query("attention")
    assert len(result) == 1
    assert result[0]["entity"].entity_id == "e1"
    assert {r.rel_id for r in result[0]["relations"]} == {"r1", "r2"}

    reborn = _model(tmp_path)
    again = reborn.query("attention")
    assert len(again) == 1
    assert {r.rel_id for r in again[0]["relations"]} == {"r1", "r2"}
    assert reborn.entity_count == 3
    assert reborn.relation_count == 2


def test_reads_are_open_writes_are_gated(tmp_path: Path) -> None:
    """The discriminator.

    Shared mutable knowledge with unrestricted writes lets one confused agent
    poison every other agent's reasoning — permanently, since the graph outlives
    the process. Reading must stay open so the graph is useful.
    """
    wm = _model(tmp_path)
    wm.add_entity(Entity("e1", "attention", "concept"), agent_ddr=WRITER)

    # A read-only agent can query freely...
    assert wm.query("attention")[0]["entity"].name == "attention"
    assert wm.get("e1") is not None
    assert wm.can_write(READER) is False

    # ...but cannot write.
    with pytest.raises(CapabilityError, match="CAP_WORLD_MODEL_WRITE"):
        wm.add_entity(Entity("e9", "injected", "concept"), agent_ddr=READER)
    with pytest.raises(CapabilityError):
        wm.update_entity("e1", {"poisoned": True}, agent_ddr=READER)
    with pytest.raises(CapabilityError):
        wm.add_relation(Relation("r9", "e1", "e1", "self"), agent_ddr=READER)
    assert wm.entity_count == 1


def test_lower_confidence_cannot_overwrite(tmp_path: Path) -> None:
    """Last-write-wins would let a guess clobber a well-evidenced fact."""
    wm = _model(tmp_path)
    wm.add_entity(
        Entity("e1", "attention", "concept", {"origin": "2017"}, confidence=0.9),
        agent_ddr=WRITER,
    )
    wm.update_entity("e1", {"origin": "1066"}, agent_ddr=WRITER, confidence=0.2)
    assert wm.get("e1").properties["origin"] == "2017"     # type: ignore[union-attr]
    assert wm.get("e1").confidence == pytest.approx(0.9)   # type: ignore[union-attr]

    # A better-evidenced update does land.
    wm.update_entity("e1", {"origin": "2017-06"}, agent_ddr=WRITER, confidence=0.95)
    assert wm.get("e1").properties["origin"] == "2017-06"  # type: ignore[union-attr]


def test_relation_endpoints_must_exist(tmp_path: Path) -> None:
    """A dangling relation is a fact about nothing."""
    wm = _model(tmp_path)
    wm.add_entity(Entity("e1", "attention"), agent_ddr=WRITER)
    with pytest.raises(WorldModelError, match="endpoint not in graph"):
        wm.add_relation(Relation("r1", "e1", "ghost", "uses"), agent_ddr=WRITER)


def test_neighbours_and_type_filter(tmp_path: Path) -> None:
    wm = _model(tmp_path)
    for eid, name in (("e1", "attention"), ("e2", "transformer"), ("e3", "softmax")):
        wm.add_entity(Entity(eid, name), agent_ddr=WRITER)
    wm.add_relation(Relation("r1", "e1", "e2", "part_of"), agent_ddr=WRITER)
    wm.add_relation(Relation("r2", "e1", "e3", "uses"), agent_ddr=WRITER)

    assert [e.entity_id for e in wm.neighbours("e1")] == ["e2", "e3"]
    assert [e.entity_id for e in wm.neighbours("e1", rel_type="uses")] == ["e3"]
    # Relations are undirected for neighbour lookup.
    assert [e.entity_id for e in wm.neighbours("e2")] == ["e1"]


def test_entity_validation(tmp_path: Path) -> None:
    wm = _model(tmp_path)
    with pytest.raises(WorldModelError, match="id and a name"):
        wm.add_entity(Entity("", "nameless"), agent_ddr=WRITER)
    with pytest.raises(WorldModelError, match="confidence"):
        Entity("e", "n", confidence=1.5)


def test_update_unknown_entity(tmp_path: Path) -> None:
    wm = _model(tmp_path)
    with pytest.raises(WorldModelError, match="unknown entity"):
        wm.update_entity("ghost", {"x": 1}, agent_ddr=WRITER)


def test_confidence_range_on_update(tmp_path: Path) -> None:
    wm = _model(tmp_path)
    wm.add_entity(Entity("e1", "attention"), agent_ddr=WRITER)
    with pytest.raises(WorldModelError, match="out of range"):
        wm.update_entity("e1", {}, agent_ddr=WRITER, confidence=2.0)


def test_query_miss_returns_empty(tmp_path: Path) -> None:
    assert _model(tmp_path).query("nothing here") == []


def test_writes_are_audited(tmp_path: Path) -> None:
    wm = _model(tmp_path)
    wm.add_entity(Entity("e1", "attention"), agent_ddr=WRITER)
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "D-04" and e["action"] == "world.add_entity" for e in entries)


def test_corrupt_store_reported(tmp_path: Path) -> None:
    (tmp_path / "entities.jsonl").write_text("{bad\n", encoding="utf-8")
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    with pytest.raises(WorldModelError, match="corrupt entity store"):
        WorldModel(
            lockbox=lb,
            entities_path=tmp_path / "entities.jsonl",
            relations_path=tmp_path / "relations.jsonl",
            audit=audit,
        )
