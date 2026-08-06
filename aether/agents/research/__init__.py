"""D-07 research agents: hypothesis generation and experiment evaluation.

Genome lineage for research agents (Section 3D #61, DDR-851/855).

WHAT IS **NOT** HERE, AND WHY. An earlier draft of this module (DDR-853) also
defined a `Hypothesis`/`HypothesisTree` and a `DeadEnd`/`DeadEndRegistry`. Both
duplicated code that already existed and was stricter:

  * `hypothesis_generator.Hypothesis` (D-07) requires statement ·
    falsification_condition · expected_evidence · estimated_cost. The draft
    required only a `prediction`, so importing the wrong one would have silently
    weakened what counts as a hypothesis.
  * `memory.failure_memory_registry.FailureMemoryRegistry` (D-13) is append-only
    with **no delete path at all**. The draft evicted its oldest entries, which
    is precisely the forgetting D-13 exists to make impossible.

The tree lives in `hypothesis_tree.py` and now wraps D-07's type; the dead-end
divergence score was added to D-13 rather than reimplemented. DDR-855 records
the duplication and its cause.
"""

from __future__ import annotations

from dataclasses import dataclass

__all__ = ["ResearchError", "Genome", "GenomeArchive"]

DDR_ID = "D-07"


class ResearchError(Exception):
    """A research-lineage operation that must not proceed."""


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
