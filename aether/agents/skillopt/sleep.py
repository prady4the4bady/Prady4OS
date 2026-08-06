"""SkillOpt-Sleep — offline consolidation (Section 3D #47, DDR-848).

During waking operation an agent records rollouts and does not revise itself:
rewriting a skill mid-task means the thing being measured changes while it is
being measured. Consolidation happens later, in a bounded idle pass — "sleep".

THE RULE THAT MAKES THIS SAFE: sleep uses the SAME acceptance bar as the waking
loop. There is no relaxed offline threshold, no "we have more data now so a tie
is good enough". An offline pass is a better opportunity to be rigorous, not a
licence to be lenient, and a second weaker acceptance path would become the one
every change flows through.

Two further properties this module exists to provide:

- **The train/held-out split is deterministic and arrival-order independent.**
  Splitting by insertion order correlates the held-out set with time, so a skill
  that got better mid-session scores well for reasons unrelated to the edit.
  Splitting randomly makes a rejection impossible to reproduce, and an
  unreproducible rejection gets re-run until it passes.
- **Consolidation is all-or-nothing.** An interrupted pass leaves the incumbent
  exactly as it was. A half-applied skill revision is a document no one wrote.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from typing import Callable, Sequence

from . import MIN_HELDOUT_ROLLOUTS, Rollout, SkillOptLoop, SkillOptResult
from .validate import SkillValidationError, validate_skill_update

__all__ = [
    "SleepResult",
    "SkillOptSleep",
    "heldout_split",
    "MIN_TRAIN_ROLLOUTS",
    "JOURNAL_CAPACITY",
]

# Below this there is nothing to learn FROM, as distinct from nothing to score
# against. Both floors are needed: a large held-out set over 1 training rollout
# scores a candidate built from a single anecdote.
MIN_TRAIN_ROLLOUTS = 4

# The journal is bounded. An unbounded rollout log is a memory leak that only
# manifests on long-lived agents, which is to say the ones that matter (S2).
JOURNAL_CAPACITY = 4096

# Fraction of rollouts held out. One third, so the held-out set reaches its
# minimum at roughly the same size the training set reaches its own.
_HELDOUT_NUMERATOR = 1
_HELDOUT_DENOMINATOR = 3


def heldout_split(
    rollouts: Sequence[Rollout], salt: str = ""
) -> tuple[list[Rollout], list[Rollout]]:
    """Partition into (train, heldout) deterministically by task_id.

    Keyed on a hash of the task_id rather than position, so the same rollout
    lands on the same side no matter when it arrived or how the list was
    ordered. `salt` lets a caller take a genuinely different split when it has
    a reason to; the default of "" keeps a given agent's split stable across
    runs, which is what makes a rejection reproducible.
    """
    train: list[Rollout] = []
    held: list[Rollout] = []
    for r in rollouts:
        digest = hashlib.sha256(f"{salt}:{r.task_id}".encode("utf-8")).digest()
        bucket = digest[0] % _HELDOUT_DENOMINATOR
        (held if bucket < _HELDOUT_NUMERATOR else train).append(r)
    return train, held


@dataclass
class SleepResult:
    """Outcome of one consolidation pass.

    `applied` is False for every refusal — too little data, validation failure,
    and a candidate that merely tied are all reported the same way, because to
    the operator they mean the same thing: the skill did not change and here is
    why.
    """

    applied: bool
    reason: str
    train_n: int
    heldout_n: int
    detail: SkillOptResult | None = None


class SkillOptSleep:
    """Journals rollouts while awake; consolidates in a bounded idle pass."""

    def __init__(
        self,
        incumbent: str,
        scorer: Callable[[str, Rollout], float],
        granted_caps: frozenset[str] | set[str] | None = None,
        capacity: int = JOURNAL_CAPACITY,
    ) -> None:
        if capacity <= 0:
            raise ValueError("journal capacity must be positive")
        self._loop = SkillOptLoop(incumbent, scorer)
        self._granted = frozenset(granted_caps) if granted_caps is not None else None
        self._capacity = capacity
        self._journal: list[Rollout] = []
        self.dropped = 0
        # Section 3D #47: sleep PAUSES ACTIVE AGENTS. Tracked explicitly rather
        # than assumed, because "the caller will have stopped them" is exactly
        # the assumption that is true right up until it is not.
        self._active: set[str] = set()
        self.paused = False

    # -- agent activity, so sleep can refuse to run over live work ---------
    def mark_active(self, agent: str) -> None:
        self._active.add(agent)

    def mark_idle(self, agent: str) -> None:
        self._active.discard(agent)

    @property
    def active_agents(self) -> frozenset[str]:
        return frozenset(self._active)

    def pause_agents(self) -> frozenset[str]:
        """Quiesce before consolidating. Returns who was paused."""
        paused = frozenset(self._active)
        self._active.clear()
        self.paused = True
        return paused

    def resume_agents(self, agents: frozenset[str] | set[str]) -> None:
        self._active |= set(agents)
        self.paused = False

    @property
    def incumbent(self) -> str:
        return self._loop.incumbent

    @property
    def journal(self) -> tuple[Rollout, ...]:
        return tuple(self._journal)

    def record(self, rollout: Rollout) -> None:
        """Journal a rollout. Never revises the skill — that is sleep's job."""
        self._journal.append(rollout)
        if len(self._journal) > self._capacity:
            # Drop the OLDEST. Counted, not silent: a consolidation run over a
            # journal that overflowed saw less than the agent actually did, and
            # an operator reading the result needs to know that.
            self._journal.pop(0)
            self.dropped += 1

    # -- harvest -> mine -> replay -> consolidate --------------------------
    def harvest(self, salt: str = "") -> tuple[list[Rollout], list[Rollout]]:
        """Stage 1. Gather the journal and split it train/held-out."""
        return heldout_split(self._journal, salt)

    @staticmethod
    def mine(train: Sequence[Rollout], k: int = 3):
        """Stage 2. Turn rollouts into the k heaviest lessons."""
        return SkillOptLoop.select(SkillOptLoop.aggregate(SkillOptLoop.reflect(train)), k)

    def replay(self, candidate, held: Sequence[Rollout]):
        """Stage 3. Score incumbent and candidate over the held-out set."""
        return self._loop.evaluate(candidate, held)

    def consolidate(
        self,
        salt: str = "",
        k: int = 3,
        approver_caps: frozenset[str] | set[str] | None = None,
    ) -> SleepResult:
        """Stages 1-4. The incumbent changes only on a clean acceptance.

        Refuses to run while any agent is active. Consolidating under live work
        rewrites the skill of an agent that is mid-task — the revision lands
        between two steps of one action, so neither the old skill nor the new
        one describes what actually ran, and the trajectory cannot be read back
        as a single coherent run.
        """
        if self._active:
            return SleepResult(
                False,
                f"agents still active: {sorted(self._active)} — sleep pauses "
                "active agents before consolidating (3D #47). Rewriting a skill "
                "mid-task means neither the old nor the new skill describes what "
                "ran",
                0, 0,
            )

        train, held = self.harvest(salt)

        if len(train) < MIN_TRAIN_ROLLOUTS:
            return SleepResult(
                False,
                f"only {len(train)} training rollouts (need {MIN_TRAIN_ROLLOUTS}) "
                "— nothing to learn from yet",
                len(train), len(held),
            )
        if len(held) < MIN_HELDOUT_ROLLOUTS:
            return SleepResult(
                False,
                f"only {len(held)} held-out rollouts (need {MIN_HELDOUT_ROLLOUTS}) "
                "— refusing to shrink the held-out set to make a pass possible",
                len(train), len(held),
            )

        # Build the candidate WITHOUT touching the incumbent, so validation runs
        # before anything is scored. A candidate that escalates capability must
        # never reach a comparison a good score could carry it through.
        candidate = SkillOptLoop.update(self.incumbent, self.mine(train, k))
        try:
            validate_skill_update(
                self.incumbent, candidate.body, self._granted, approver_caps
            )
        except SkillValidationError as exc:
            return SleepResult(
                False, f"candidate rejected by validation: {exc}",
                len(train), len(held),
            )

        result = self.replay(candidate, held)
        if result.accepted:
            self._loop.incumbent = result.skill_body
        self._loop.history.append(result)

        return SleepResult(
            result.accepted, result.reason, len(train), len(held), result
        )
