"""D-07 hypothesis TREE — versioning, lineage, persistence (3D #60, DDR-855).

Spec: *"hypothesis tree in SFS (versioned, persists across boots)"*.

**This does not define a hypothesis.** `hypothesis_generator.Hypothesis` (D-07)
already does, and does it more strictly than a second type would: statement ·
falsification_condition · expected_evidence · estimated_cost, every one
required. This module stores *those* objects and adds only what D-07 does not
have — parent links, versions, and a stable serialisation that survives a
reboot.

That division matters. An earlier draft of this module declared its own
`Hypothesis` with a single `prediction` field, which would have meant two
incompatible notions of what a hypothesis is, and the weaker one silently
winning wherever it was imported. DDR-855 records how that happened.

**Superseding creates a new node; the old one remains.** Editing in place erases
the reasoning that led somewhere wrong, which is the only thing that stops the
agent walking back down it — and D-07's dead-end pre-check can only match what
is still on the record.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from .hypothesis_generator import Hypothesis, HypothesisError, HypothesisStatus

__all__ = ["TreeNode", "HypothesisTree", "TreeError"]

DDR_ID = "D-07"
SCHEMA_VERSION = 1


class TreeError(HypothesisError):
    """A tree operation that must not proceed."""


@dataclass(frozen=True)
class TreeNode:
    """One versioned position in the tree, wrapping a D-07 hypothesis."""

    hypothesis: Hypothesis
    version: int = 1
    parent: str | None = None
    evidence: str = ""

    @property
    def hyp_id(self) -> str:
        return self.hypothesis.hyp_id


@dataclass
class HypothesisTree:
    """Append-only, versioned, serialisable.

    Insertion order is preserved because it is load-bearing: a parent must exist
    before its child is added and no node is ever re-parented, so every edge
    points strictly backwards and `lineage()` terminates by construction. There
    is deliberately no cycle check — it would be unreachable, and dead code that
    looks like a safety net invites trusting a guard that has never run.
    """

    _nodes: dict[str, TreeNode] = field(default_factory=dict)
    _order: list[str] = field(default_factory=list)

    def add(self, hypothesis: Hypothesis, parent: str | None = None) -> TreeNode:
        hid = hypothesis.hyp_id
        if hid in self._nodes:
            raise TreeError(
                f"{hid} already in the tree. Hypotheses are never edited in "
                "place — supersede so the reasoning that led somewhere wrong "
                "stays readable, and so the dead-end registry can still match it"
            )
        if parent is not None and parent not in self._nodes:
            raise TreeError(f"{hid}: parent {parent!r} is not in the tree")
        node = TreeNode(hypothesis, 1 if parent is None else
                        self._nodes[parent].version + 1, parent)
        self._nodes[hid] = node
        self._order.append(hid)
        return node

    def supersede(self, hyp_id: str, replacement: Hypothesis) -> TreeNode:
        """Add `replacement` as the next version. The old node REMAINS."""
        if hyp_id not in self._nodes:
            raise TreeError(f"no hypothesis {hyp_id!r} to supersede")
        if replacement.hyp_id == hyp_id:
            raise TreeError(
                "the replacement must have its own id — reusing the id makes "
                "the new version indistinguishable from an in-place edit"
            )
        return self.add(replacement, parent=hyp_id)

    def resolve(self, hyp_id: str, status: HypothesisStatus, evidence: str) -> TreeNode:
        """Record a terminal outcome. Evidence is required, refutations included."""
        if status in (HypothesisStatus.PROPOSED, HypothesisStatus.QUEUED):
            raise TreeError("resolve requires a terminal status")
        if not evidence.strip():
            raise TreeError(
                f"{hyp_id}: resolution requires evidence. An unevidenced verdict "
                "is an opinion recorded in the shape of a result"
            )
        node = self.get(hyp_id)
        if node.hypothesis.status not in (HypothesisStatus.PROPOSED,
                                          HypothesisStatus.QUEUED):
            raise TreeError(
                f"{hyp_id} is already {node.hypothesis.status.value} — "
                "re-resolving would overwrite the first verdict and its evidence"
            )
        h = node.hypothesis
        updated = Hypothesis(
            h.hyp_id, h.statement, h.falsification_condition, h.expected_evidence,
            h.estimated_cost, h.origin, status, h.ts,
        )
        new_node = TreeNode(updated, node.version, node.parent, evidence)
        self._nodes[hyp_id] = new_node
        return new_node

    def get(self, hyp_id: str) -> TreeNode:
        if hyp_id not in self._nodes:
            raise TreeError(f"no hypothesis {hyp_id!r}")
        return self._nodes[hyp_id]

    def children(self, hyp_id: str) -> list[TreeNode]:
        return [self._nodes[k] for k in self._order
                if self._nodes[k].parent == hyp_id]

    def lineage(self, hyp_id: str) -> list[str]:
        out: list[str] = []
        cur: str | None = hyp_id
        while cur is not None:
            out.append(cur)
            cur = self._nodes[cur].parent
        return list(reversed(out))

    @property
    def falsified(self) -> list[TreeNode]:
        """Retained on purpose — these are what the dead-end registry feeds on."""
        return [self._nodes[k] for k in self._order
                if self._nodes[k].hypothesis.status is HypothesisStatus.FALSIFIED]

    def to_dict(self) -> dict:
        return {
            "schema": SCHEMA_VERSION,
            "nodes": [
                {
                    "hyp_id": n.hypothesis.hyp_id,
                    "statement": n.hypothesis.statement,
                    "falsification_condition": n.hypothesis.falsification_condition,
                    "expected_evidence": n.hypothesis.expected_evidence,
                    "estimated_cost": n.hypothesis.estimated_cost,
                    "origin": n.hypothesis.origin,
                    "status": n.hypothesis.status.value,
                    "ts": n.hypothesis.ts,
                    "version": n.version,
                    "parent": n.parent,
                    "evidence": n.evidence,
                }
                for n in (self._nodes[k] for k in self._order)
            ],
        }

    @classmethod
    def from_dict(cls, data: dict) -> "HypothesisTree":
        if data.get("schema") != SCHEMA_VERSION:
            raise TreeError(
                f"unknown hypothesis tree schema {data.get('schema')!r} — "
                "refusing to guess at a format this build does not know, which "
                "is how a persisted structure silently loses fields"
            )
        tree = cls()
        for n in data["nodes"]:
            h = Hypothesis(
                n["hyp_id"], n["statement"], n["falsification_condition"],
                n["expected_evidence"], n["estimated_cost"], n["origin"],
                HypothesisStatus(n["status"]), n["ts"],
            )
            node = TreeNode(h, n["version"], n["parent"], n["evidence"])
            tree._nodes[h.hyp_id] = node
            tree._order.append(h.hyp_id)
        return tree
