"""Vector knowledge graph memory (Section 3D #62, DDR-856).

Spec: *"vector knowledge graph memory (RuVector-style, online learning)"*.

Placed in `agents/memory/` beside D-11 rather than in a new package, because it
is memory. It does **not** overlap D-11: `knowledge_consolidation` resolves
*conflicting claims* about the same key; this stores *embedded nodes and typed
edges* and answers similarity queries. Different questions, so they compose —
D-11 decides which belief is true, this one finds which beliefs are related.

Four refusals carry the design:

- **A dimension mismatch is refused, never padded or truncated.** Padding a
  short vector with zeros silently changes its direction, so the nearest
  neighbour it returns is confidently wrong rather than absent.
- **A zero vector is refused.** Cosine similarity against it is undefined;
  returning 0.0 would assert "maximally dissimilar to everything", which is a
  claim the data does not support.
- **Adding to a full graph raises**, it does not evict. D-13 established the
  principle for failure memory and it holds here: silent forgetting turns a
  store into something that cannot be reasoned about. The message points at
  consolidation, which is a decision the operator makes.
- **An edge needs both endpoints.** A dangling edge is a relation to something
  that is not there, which every traversal has to special-case or trip over.

"Online learning" means `reinforce()` moves a stored embedding toward new
evidence incrementally, with no reindex — the RuVector property the spec names.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

__all__ = [
    "VectorGraphError",
    "Node",
    "Edge",
    "VectorGraph",
    "cosine",
    "DEFAULT_CAPACITY",
]

DDR_ID = "D-11"
DEFAULT_CAPACITY = 4096


class VectorGraphError(ValueError):
    """A graph operation that must not proceed."""


def cosine(a: tuple[float, ...], b: tuple[float, ...]) -> float:
    """Cosine similarity in [-1, 1]. Raises on a zero vector.

    Raising rather than returning 0.0 is the decision: 0.0 is a real similarity
    value meaning orthogonal, so using it for "undefined" makes an error
    indistinguishable from a legitimate result.
    """
    if len(a) != len(b):
        raise VectorGraphError(f"dimension mismatch: {len(a)} vs {len(b)}")
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(x * x for x in b))
    if na == 0.0 or nb == 0.0:
        raise VectorGraphError(
            "cosine similarity against a zero vector is undefined. Refusing to "
            "return 0.0 — that is a real similarity meaning orthogonal, and "
            "using it here would make an error look like a result"
        )
    return sum(x * y for x, y in zip(a, b)) / (na * nb)


@dataclass
class Node:
    key: str
    vector: tuple[float, ...]
    label: str = ""
    updates: int = 0


@dataclass(frozen=True)
class Edge:
    src: str
    dst: str
    relation: str
    weight: float = 1.0


@dataclass
class VectorGraph:
    """Embedded nodes plus typed edges, with incremental online updates."""

    dim: int
    capacity: int = DEFAULT_CAPACITY
    _nodes: dict[str, Node] = field(default_factory=dict)
    _edges: list[Edge] = field(default_factory=list)

    def __post_init__(self) -> None:
        if self.dim <= 0:
            raise VectorGraphError("dimension must be positive")
        if self.capacity <= 0:
            raise VectorGraphError("capacity must be positive")

    # -- validation --------------------------------------------------------
    def _check(self, vector: tuple[float, ...]) -> tuple[float, ...]:
        if len(vector) != self.dim:
            raise VectorGraphError(
                f"vector has {len(vector)} dimensions, graph has {self.dim}. "
                "Refusing to pad or truncate: either one silently changes the "
                "vector's direction, so the nearest neighbour it returns is "
                "confidently wrong rather than absent"
            )
        if any(math.isnan(x) or math.isinf(x) for x in vector):
            raise VectorGraphError("vector contains NaN or infinity")
        if all(x == 0.0 for x in vector):
            raise VectorGraphError(
                "refusing a zero vector: it has no direction, so every "
                "similarity query against it is undefined"
            )
        return tuple(float(x) for x in vector)

    # -- nodes -------------------------------------------------------------
    def add(self, key: str, vector: tuple[float, ...], label: str = "") -> Node:
        if not key.strip():
            raise VectorGraphError("node requires a key")
        if key in self._nodes:
            raise VectorGraphError(
                f"node {key!r} already exists — use reinforce() to update it, so "
                "the update count stays honest about how much evidence there is"
            )
        if len(self._nodes) >= self.capacity:
            raise VectorGraphError(
                f"graph is full ({self.capacity} nodes). Refusing to evict: "
                "silent forgetting turns a memory into something that cannot be "
                "reasoned about (the D-13 principle). Consolidate first — that "
                "is a decision for the operator, not a side effect of an insert"
            )
        node = Node(key, self._check(vector), label)
        self._nodes[key] = node
        return node

    def reinforce(
        self, key: str, vector: tuple[float, ...], rate: float = 0.2
    ) -> Node:
        """Online update: move the stored embedding toward new evidence.

        Incremental by design — no reindex, which is the RuVector property the
        spec asks for. `rate` is bounded to (0, 1]: 0 would be a silent no-op
        that looks like an update, and above 1 would overshoot past the new
        evidence to somewhere neither observation supports.
        """
        if not 0.0 < rate <= 1.0:
            raise VectorGraphError(f"rate must be in (0,1], got {rate}")
        node = self.get(key)
        new = self._check(vector)
        blended = tuple(
            (1.0 - rate) * old + rate * fresh for old, fresh in zip(node.vector, new)
        )
        if all(x == 0.0 for x in blended):
            raise VectorGraphError(
                "reinforcement cancelled the embedding to zero — the update is "
                "refused rather than leaving a node no query can ever match"
            )
        node.vector = blended
        node.updates += 1
        return node

    def get(self, key: str) -> Node:
        if key not in self._nodes:
            raise VectorGraphError(f"no node {key!r}")
        return self._nodes[key]

    def __len__(self) -> int:
        return len(self._nodes)

    @property
    def nodes(self) -> tuple[Node, ...]:
        """Insertion order, deliberately NOT sorted.

        An earlier draft sorted here *and* broke ties by key in `nearest()`.
        Two mechanisms for one guarantee meant the tie-break was unreachable
        dead code — mutation testing caught it surviving, because with a
        pre-sorted input and a stable sort, removing it changed nothing. The
        ordering guarantee now lives in exactly one place: `nearest()` sorts
        explicitly, so the code that provides determinism is the code a test
        can kill.
        """
        return tuple(self._nodes.values())

    # -- edges -------------------------------------------------------------
    def link(self, src: str, dst: str, relation: str, weight: float = 1.0) -> Edge:
        for endpoint in (src, dst):
            if endpoint not in self._nodes:
                raise VectorGraphError(
                    f"cannot link: {endpoint!r} is not in the graph. A dangling "
                    "edge is a relation to something that is not there, which "
                    "every traversal then has to special-case or trip over"
                )
        if not relation.strip():
            raise VectorGraphError("an edge requires a relation type")
        edge = Edge(src, dst, relation, weight)
        if edge in self._edges:
            raise VectorGraphError(f"duplicate edge {src}-[{relation}]->{dst}")
        self._edges.append(edge)
        return edge

    def neighbours(self, key: str, relation: str | None = None) -> list[Edge]:
        self.get(key)
        return [
            e for e in self._edges
            if e.src == key and (relation is None or e.relation == relation)
        ]

    @property
    def edges(self) -> tuple[Edge, ...]:
        return tuple(self._edges)

    # -- query -------------------------------------------------------------
    def nearest(
        self, vector: tuple[float, ...], k: int = 5, threshold: float = -1.0
    ) -> list[tuple[Node, float]]:
        """The k most similar nodes, most similar first.

        Ties break on node key so the same query returns the same order every
        time. A ranking that depends on dict insertion order makes a bad recall
        impossible to reproduce, and an irreproducible bad result gets retried
        rather than investigated.
        """
        if k <= 0:
            raise VectorGraphError("k must be positive")
        probe = self._check(vector)
        scored = [(node, cosine(probe, node.vector)) for node in self.nodes]
        scored = [(n, s) for n, s in scored if s >= threshold]
        scored.sort(key=lambda pair: (-pair[1], pair[0].key))
        return scored[:k]
