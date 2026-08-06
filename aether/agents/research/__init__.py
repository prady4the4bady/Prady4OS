"""D-07 research agents: hypothesis generation and experiment evaluation.

Hypothesis tree, genome lineage, dead-end registry (3D #60/#61/#63, DDR-853).
This module was a docstring-only stub carrying the D-07 label; that label is
kept above because it is the only link between this package and the D-07
tracker entry, and losing it would orphan both.

Spec: *"hypothesis tree in SFS (versioned, persists across boots) · `genome.md`
per research agent (lineage archived) · dead-end registry (failure reason +
divergence score, queried before repeating)"*.

All three are **records of things that did not work**, and that is the point.
A research agent's failures are its most valuable output and the easiest to
lose: a refuted hypothesis looks like clutter, a superseded genome looks
obsolete, a dead end looks like noise. Delete them and the agent re-derives the
same wrong answer indefinitely, each time at full cost, with nothing in the
record explaining why it keeps happening.

So every structure here is **append-only**, and every one retains what failed.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

__all__ = [
    "ResearchError",
    "HypothesisStatus",
    "Hypothesis",
    "HypothesisTree",
    "Genome",
    "GenomeArchive",
    "DeadEnd",
    "DeadEndRegistry",
    "divergence",
    "MIN_DIVERGENCE",
]


class ResearchError(Exception):
    pass


# ==========================================================================
# #60 hypothesis tree
# ==========================================================================
class HypothesisStatus(Enum):
    OPEN = "open"
    SUPPORTED = "supported"
    REFUTED = "refuted"


@dataclass(frozen=True)
class Hypothesis:
    """One versioned node.

    `prediction` is required and must be falsifiable-shaped for the same reason
    a goal needs a success criterion: a hypothesis that cannot be wrong cannot
    be right either, and an experiment against it will "confirm" it whatever
    happens.
    """

    hid: str
    statement: str
    prediction: str
    version: int = 1
    parent: str | None = None
    status: HypothesisStatus = HypothesisStatus.OPEN
    evidence: str = ""

    def __post_init__(self) -> None:
        for field_name in ("hid", "statement", "prediction"):
            if not getattr(self, field_name).strip():
                raise ResearchError(
                    f"hypothesis requires {field_name}. A hypothesis with no "
                    "prediction cannot be wrong, so it cannot be right either — "
                    "an experiment against it confirms it whatever happens"
                )
        if self.version < 1:
            raise ResearchError("version starts at 1")


class HypothesisTree:
    """Versioned, append-only. Persists by serialising to a stable dict."""

    def __init__(self) -> None:
        self._nodes: dict[str, Hypothesis] = {}
        self._order: list[str] = []

    def add(self, h: Hypothesis) -> Hypothesis:
        if h.hid in self._nodes:
            raise ResearchError(
                f"{h.hid} already exists. Hypotheses are never edited in place — "
                "supersede it with a new version so the reasoning that led "
                "somewhere wrong stays readable"
            )
        if h.parent is not None and h.parent not in self._nodes:
            raise ResearchError(f"{h.hid}: parent {h.parent!r} does not exist")
        self._nodes[h.hid] = h
        self._order.append(h.hid)
        return h

    # WHY THERE IS NO CYCLE CHECK. Two invariants make one unreachable: a
    # parent must already exist when its child is added, and a node is never
    # re-parented (add refuses a duplicate hid, and supersede creates a NEW
    # node). Every edge therefore points strictly backwards in insertion order,
    # so `lineage()` terminates by construction.
    #
    # An explicit cycle check was written here and removed once it was clear no
    # input could reach it. Dead code that looks like a safety net is worse
    # than no net: it invites the reader to trust a guard that has never run
    # and would not be noticed if it broke.

    def supersede(self, hid: str, statement: str, prediction: str) -> Hypothesis:
        """Create the next version. The old one REMAINS.

        Editing in place would erase the reasoning that led somewhere wrong,
        which is the only thing that stops the agent walking back down it.
        """
        old = self.get(hid)
        new = Hypothesis(
            hid=f"{hid}.v{old.version + 1}",
            statement=statement,
            prediction=prediction,
            version=old.version + 1,
            parent=hid,
        )
        return self.add(new)

    def resolve(self, hid: str, status: HypothesisStatus, evidence: str) -> Hypothesis:
        """Record an outcome. Requires evidence, including for a refutation."""
        if status is HypothesisStatus.OPEN:
            raise ResearchError("resolve requires a terminal status")
        if not evidence.strip():
            raise ResearchError(
                f"{hid}: resolution requires evidence. An unevidenced verdict is "
                "an opinion recorded in the shape of a result"
            )
        old = self.get(hid)
        if old.status is not HypothesisStatus.OPEN:
            raise ResearchError(
                f"{hid} is already {old.status.value} — re-resolving would "
                "overwrite the first verdict and the evidence behind it"
            )
        updated = Hypothesis(
            old.hid, old.statement, old.prediction, old.version, old.parent,
            status, evidence,
        )
        self._nodes[hid] = updated
        return updated

    def get(self, hid: str) -> Hypothesis:
        if hid not in self._nodes:
            raise ResearchError(f"no hypothesis {hid!r}")
        return self._nodes[hid]

    def children(self, hid: str) -> list[Hypothesis]:
        return [self._nodes[k] for k in self._order if self._nodes[k].parent == hid]

    def lineage(self, hid: str) -> list[str]:
        """Root-first path to `hid`."""
        out: list[str] = []
        cur: str | None = hid
        while cur is not None:
            out.append(cur)
            cur = self._nodes[cur].parent
        return list(reversed(out))

    @property
    def refuted(self) -> list[Hypothesis]:
        return [self._nodes[k] for k in self._order
                if self._nodes[k].status is HypothesisStatus.REFUTED]

    def to_dict(self) -> dict:
        """Stable serialisation — insertion order, for persistence across boots."""
        return {
            "version": 1,
            "nodes": [
                {
                    "hid": h.hid, "statement": h.statement, "prediction": h.prediction,
                    "version": h.version, "parent": h.parent,
                    "status": h.status.value, "evidence": h.evidence,
                }
                for h in (self._nodes[k] for k in self._order)
            ],
        }

    @classmethod
    def from_dict(cls, data: dict) -> "HypothesisTree":
        if data.get("version") != 1:
            raise ResearchError(
                f"unknown hypothesis tree version {data.get('version')!r} — "
                "refusing to guess at a format this build does not know"
            )
        tree = cls()
        for n in data["nodes"]:
            tree.add(Hypothesis(
                n["hid"], n["statement"], n["prediction"], n["version"],
                n["parent"], HypothesisStatus(n["status"]), n["evidence"],
            ))
        return tree


# ==========================================================================
# #61 genome.md per research agent
# ==========================================================================
@dataclass(frozen=True)
class Genome:
    """One generation of a research agent's configuration."""

    agent: str
    generation: int
    traits: dict[str, str]
    parent_generation: int | None = None
    rationale: str = ""

    def __post_init__(self) -> None:
        if not self.agent.strip():
            raise ResearchError("genome requires an agent")
        if self.generation < 0:
            raise ResearchError("generation must not be negative")
        if not self.traits:
            raise ResearchError("genome requires at least one trait")
        if self.generation > 0 and not self.rationale.strip():
            raise ResearchError(
                f"generation {self.generation} requires a rationale. A mutation "
                "with no stated reason cannot be evaluated later — the lineage "
                "records that something changed and not why, which is the part "
                "that would tell you whether to do it again"
            )

    def diff(self, other: "Genome") -> dict[str, tuple[str | None, str | None]]:
        keys = set(self.traits) | set(other.traits)
        return {
            k: (self.traits.get(k), other.traits.get(k))
            for k in sorted(keys)
            if self.traits.get(k) != other.traits.get(k)
        }


class GenomeArchive:
    """Append-only lineage. A superseded genome is never deleted."""

    def __init__(self, agent: str) -> None:
        if not agent.strip():
            raise ResearchError("archive requires an agent")
        self.agent = agent
        self._generations: list[Genome] = []

    def seed(self, traits: dict[str, str]) -> Genome:
        if self._generations:
            raise ResearchError("archive is already seeded")
        g = Genome(self.agent, 0, dict(traits))
        self._generations.append(g)
        return g

    def evolve(self, traits: dict[str, str], rationale: str) -> Genome:
        """Add the next generation, linked to its parent."""
        if not self._generations:
            raise ResearchError("archive must be seeded before it can evolve")
        parent = self._generations[-1]
        g = Genome(self.agent, parent.generation + 1, dict(traits),
                   parent.generation, rationale)
        if not g.diff(parent):
            raise ResearchError(
                "evolve produced no trait change — an empty generation makes "
                "the lineage longer without making it more informative"
            )
        self._generations.append(g)
        return g

    @property
    def current(self) -> Genome:
        if not self._generations:
            raise ResearchError("archive is empty")
        return self._generations[-1]

    @property
    def lineage(self) -> tuple[Genome, ...]:
        return tuple(self._generations)

    def at(self, generation: int) -> Genome:
        for g in self._generations:
            if g.generation == generation:
                return g
        raise ResearchError(f"no generation {generation}")


# ==========================================================================
# #63 dead-end registry
# ==========================================================================
# Below this, a new attempt is too close to a recorded dead end to be worth
# spending on. Tuned conservatively: a false "too similar" costs one refused
# attempt the operator can override, a false "different enough" costs the full
# cost of re-deriving a known failure.
MIN_DIVERGENCE = 0.30


def divergence(a: str, b: str) -> float:
    """1.0 - Jaccard similarity over tokens. 0.0 identical, 1.0 disjoint.

    Deliberately simple and deterministic. An embedding-based score would be
    better at judging meaning and would make a refusal impossible to explain —
    "the model thought it was similar" is not a reason an operator can argue
    with, and this refusal must be arguable.
    """
    ta = {w for w in a.lower().split() if w}
    tb = {w for w in b.lower().split() if w}
    if not ta and not tb:
        return 0.0
    if not ta or not tb:
        return 1.0
    return 1.0 - len(ta & tb) / len(ta | tb)


@dataclass(frozen=True)
class DeadEnd:
    """An approach that failed, and why."""

    approach: str
    reason: str
    cost_microcents: int = 0

    def __post_init__(self) -> None:
        if not self.approach.strip():
            raise ResearchError("dead end requires an approach")
        if not self.reason.strip():
            raise ResearchError(
                "dead end requires a failure REASON. 'It did not work' recorded "
                "without a reason cannot be checked against a new attempt, so "
                "the registry grows while remaining unable to prevent anything"
            )


@dataclass
class DeadEndRegistry:
    """Queried BEFORE an attempt, not consulted after it fails."""

    entries: list[DeadEnd] = field(default_factory=list)
    capacity: int = 1024
    dropped: int = 0

    def __post_init__(self) -> None:
        if self.capacity <= 0:
            raise ResearchError("capacity must be positive")

    def record(self, dead_end: DeadEnd) -> None:
        self.entries.append(dead_end)
        if len(self.entries) > self.capacity:
            self.entries.pop(0)
            self.dropped += 1

    def check(self, approach: str, threshold: float = MIN_DIVERGENCE) -> DeadEnd | None:
        """Return the closest recorded dead end if `approach` is too near one.

        Returns the ENTRY, not a boolean, so the caller can show the operator
        what it collided with and the reason it failed. A bare False would make
        the refusal unarguable.
        """
        if not approach.strip():
            raise ResearchError("cannot check an empty approach")
        if not 0.0 <= threshold <= 1.0:
            raise ResearchError("threshold must be in [0,1]")
        best: DeadEnd | None = None
        best_d = 1.1
        for entry in self.entries:
            d = divergence(approach, entry.approach)
            if d < best_d:
                best, best_d = entry, d
        return best if best is not None and best_d < threshold else None

    def wasted_microcents(self) -> int:
        """What the recorded dead ends already cost — the case for querying."""
        return sum(e.cost_microcents for e in self.entries)
