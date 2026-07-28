"""D-04 — persistent world model.

A semantic graph of entities and relations that survives restarts and is
readable by every agent.

The access split is the point. **Reading is open; writing needs
``CAP_WORLD_MODEL_WRITE``.** Shared mutable knowledge with unrestricted writes is
how one confused agent poisons every other agent's reasoning, and because the
graph outlives the process, that poisoning is permanent rather than a bad
afternoon.

Two supporting decisions:

* **Confidence-weighted updates.** ``update_entity`` keeps the higher-confidence
  version rather than last-write-wins, so a low-confidence guess cannot
  overwrite a well-evidenced fact just by arriving later.
* **Entities and relations are separate append-only logs**, replayed on startup
  with later records winning per id — the same derive-state-from-log discipline
  as C-04, for the same auditability reason.
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox

__all__ = [
    "Entity",
    "Relation",
    "WorldModel",
    "WorldModelError",
]

DDR_ID = "D-04"
_FILE = "aether/agents/world_model/world_model.py"
CAPABILITY = "CAP_WORLD_MODEL_WRITE"

_HERE = Path(__file__).resolve().parent
DEFAULT_ENTITIES = _HERE / "entities.jsonl"
DEFAULT_RELATIONS = _HERE / "relations.jsonl"


class WorldModelError(ValueError):
    """Graph operation refused."""


@dataclass(slots=True)
class Entity:
    entity_id: str
    name: str
    entity_type: str = ""
    properties: dict[str, Any] = field(default_factory=dict)
    confidence: float = 0.5
    ts_first_seen: float = field(default_factory=time.time)
    ts_last_updated: float = field(default_factory=time.time)

    def __post_init__(self) -> None:
        if not 0.0 <= self.confidence <= 1.0:
            raise WorldModelError(
                f"{self.entity_id}: confidence {self.confidence} out of range"
            )

    def to_json(self) -> str:
        return json.dumps(asdict(self), sort_keys=True, separators=(",", ":"))


@dataclass(slots=True)
class Relation:
    rel_id: str
    source_id: str
    target_id: str
    rel_type: str
    weight: float = 1.0
    source_agent_id: str = ""
    ts: float = field(default_factory=time.time)

    def to_json(self) -> str:
        return json.dumps(asdict(self), sort_keys=True, separators=(",", ":"))


class WorldModel:
    """Entities + relations, persisted, with write access gated."""

    def __init__(
        self,
        lockbox: Lockbox,
        entities_path: str | Path | None = None,
        relations_path: str | Path | None = None,
        audit: AuditLog | None = None,
    ) -> None:
        self.lockbox = lockbox
        self.entities_path = (
            Path(entities_path) if entities_path is not None else DEFAULT_ENTITIES
        )
        self.relations_path = (
            Path(relations_path) if relations_path is not None else DEFAULT_RELATIONS
        )
        for path in (self.entities_path, self.relations_path):
            path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self._entities: dict[str, Entity] = {}
        self._relations: dict[str, Relation] = {}
        self._replay()

    def _replay(self) -> None:
        if self.entities_path.is_file():
            with self.entities_path.open("r", encoding="utf-8") as fh:
                for raw in fh:
                    stripped = raw.strip()
                    if not stripped:
                        continue
                    try:
                        rec = json.loads(stripped)
                    except json.JSONDecodeError as exc:
                        raise WorldModelError(
                            f"corrupt entity store: {self.entities_path}"
                        ) from exc
                    self._entities[rec["entity_id"]] = Entity(**rec)
        if self.relations_path.is_file():
            with self.relations_path.open("r", encoding="utf-8") as fh:
                for raw in fh:
                    stripped = raw.strip()
                    if not stripped:
                        continue
                    try:
                        rec = json.loads(stripped)
                    except json.JSONDecodeError as exc:
                        raise WorldModelError(
                            f"corrupt relation store: {self.relations_path}"
                        ) from exc
                    self._relations[rec["rel_id"]] = Relation(**rec)

    # -- writes (capability-gated) ------------------------------------------
    def add_entity(self, entity: Entity, agent_ddr: str) -> Entity:
        self.lockbox.require(agent_ddr, CAPABILITY)
        if not entity.entity_id or not entity.name:
            raise WorldModelError("an entity needs an id and a name")
        self._entities[entity.entity_id] = entity
        with self.entities_path.open("a", encoding="utf-8") as fh:
            fh.write(entity.to_json() + "\n")
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="world.add_entity", gate_status="ok",
            entity_id=entity.entity_id, by=agent_ddr,
        )
        return entity

    def update_entity(
        self,
        entity_id: str,
        properties: dict[str, Any],
        agent_ddr: str,
        confidence: float | None = None,
    ) -> Entity:
        """Merge properties, keeping the higher-confidence view.

        Last-write-wins would let a low-confidence guess overwrite a
        well-evidenced fact purely by arriving later.
        """
        self.lockbox.require(agent_ddr, CAPABILITY)
        entity = self._entities.get(entity_id)
        if entity is None:
            raise WorldModelError(f"unknown entity: {entity_id}")
        incoming = entity.confidence if confidence is None else float(confidence)
        if not 0.0 <= incoming <= 1.0:
            raise WorldModelError(f"confidence {incoming} out of range")

        if incoming >= entity.confidence:
            entity.properties.update(properties)
            entity.confidence = incoming
            entity.ts_last_updated = time.time()
            with self.entities_path.open("a", encoding="utf-8") as fh:
                fh.write(entity.to_json() + "\n")
            status = "updated"
        else:
            status = "ignored_lower_confidence"
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="world.update_entity",
            gate_status=status, entity_id=entity_id, by=agent_ddr,
        )
        return entity

    def add_relation(self, relation: Relation, agent_ddr: str) -> Relation:
        self.lockbox.require(agent_ddr, CAPABILITY)
        for endpoint in (relation.source_id, relation.target_id):
            if endpoint not in self._entities:
                raise WorldModelError(f"relation endpoint not in graph: {endpoint}")
        self._relations[relation.rel_id] = relation
        with self.relations_path.open("a", encoding="utf-8") as fh:
            fh.write(relation.to_json() + "\n")
        return relation

    # -- reads (open to every agent) ----------------------------------------
    def get(self, entity_id: str) -> Entity | None:
        return self._entities.get(entity_id)

    def query(self, entity_name: str) -> list[dict[str, Any]]:
        """Entities matching ``entity_name`` plus their relations."""
        matches = [e for e in self._entities.values() if e.name == entity_name]
        out: list[dict[str, Any]] = []
        for entity in matches:
            related = [
                r
                for r in self._relations.values()
                if r.source_id == entity.entity_id or r.target_id == entity.entity_id
            ]
            out.append(
                {
                    "entity": entity,
                    "relations": sorted(related, key=lambda r: r.rel_id),
                }
            )
        return out

    def neighbours(self, entity_id: str, rel_type: str | None = None) -> list[Entity]:
        out: list[Entity] = []
        for rel in self._relations.values():
            if rel_type is not None and rel.rel_type != rel_type:
                continue
            other_id = (
                rel.target_id
                if rel.source_id == entity_id
                else rel.source_id
                if rel.target_id == entity_id
                else None
            )
            if other_id is None:
                continue
            other = self._entities.get(other_id)
            if other is not None:
                out.append(other)
        return sorted(out, key=lambda e: e.entity_id)

    @property
    def entity_count(self) -> int:
        return len(self._entities)

    @property
    def relation_count(self) -> int:
        return len(self._relations)

    def can_write(self, agent_ddr: str) -> bool:
        try:
            self.lockbox.require(agent_ddr, CAPABILITY)
        except CapabilityError:
            return False
        return True
