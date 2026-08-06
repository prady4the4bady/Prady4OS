"""SkillOpt — the agent skill training loop (Section 3D #46, DDR-847).

rollout -> reflect -> aggregate -> select -> update -> evaluate

THE ONLY RULE THAT MATTERS: a candidate skill replaces the incumbent **only on a
strictly positive HELD-OUT improvement**. Everything else here is bookkeeping
around that one decision.

Why the rule is written as an explicit gate rather than an implicit comparison:
an optimiser that accepts ties drifts. Each accepted tie is a coin flip that
changes the skill without evidence it improved anything, and after N ties the
skill has wandered somewhere nobody chose and no measurement supports. Requiring
`>` rather than `>=` makes drift impossible rather than merely unlikely.

Why HELD-OUT rather than training score: a skill scored on the rollouts that
produced it is scored on its own homework. That is the metric-gaming failure S7
exists to prevent, and it is the single easiest way for this loop to look like it
is working while getting worse.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Sequence

__all__ = [
    "Rollout",
    "Reflection",
    "SkillCandidate",
    "SkillOptResult",
    "SkillOptLoop",
    "MIN_HELDOUT_ROLLOUTS",
]

# A held-out score computed from too few samples is noise wearing a number's
# clothing. Below this the loop refuses to accept ANY candidate rather than
# gambling on a small sample.
MIN_HELDOUT_ROLLOUTS = 4


@dataclass(frozen=True)
class Rollout:
    """One recorded attempt by an agent at a task."""

    task_id: str
    succeeded: bool
    score: float
    notes: str = ""

    def __post_init__(self) -> None:
        if not 0.0 <= self.score <= 1.0:
            raise ValueError(f"score must be in [0,1], got {self.score}")


@dataclass(frozen=True)
class Reflection:
    """What the agent concluded from a rollout: the lesson, not the outcome."""

    task_id: str
    lesson: str
    weight: float = 1.0


@dataclass(frozen=True)
class SkillCandidate:
    """A proposed replacement skill body."""

    body: str
    derived_from: tuple[str, ...] = ()


@dataclass
class SkillOptResult:
    """The verdict, with the numbers that produced it.

    `accepted` is never reported without `incumbent_score` and
    `candidate_score`: an acceptance an operator cannot audit is a change that
    happened for unstated reasons.
    """

    accepted: bool
    reason: str
    incumbent_score: float
    candidate_score: float
    heldout_n: int
    skill_body: str

    @property
    def improvement(self) -> float:
        return self.candidate_score - self.incumbent_score


def _mean(xs: Sequence[float]) -> float:
    return sum(xs) / len(xs) if xs else 0.0


class SkillOptLoop:
    """rollout -> reflect -> aggregate -> select -> update -> evaluate.

    The loop owns the acceptance decision and nothing else. Scoring is injected,
    so a caller cannot accidentally score a candidate with a function the
    candidate influenced.
    """

    def __init__(
        self,
        incumbent: str,
        scorer: Callable[[str, Rollout], float],
        min_heldout: int = MIN_HELDOUT_ROLLOUTS,
    ) -> None:
        if not incumbent.strip():
            raise ValueError("incumbent skill body must not be empty")
        self.incumbent = incumbent
        self._scorer = scorer
        self._min_heldout = min_heldout
        # Rejections are recorded too: a loop that logs only its wins hides the
        # drift it was built to prevent.
        self.history: list[SkillOptResult] = []

    # -- reflect / aggregate / select -------------------------------------
    @staticmethod
    def reflect(rollouts: Sequence[Rollout]) -> list[Reflection]:
        """Turn rollouts into lessons.

        FAILURES ARE WEIGHTED HIGHER THAN SUCCESSES, deliberately: a success
        confirms the skill already covered the case, while a failure is the only
        signal that says where it does not.
        """
        out: list[Reflection] = []
        for r in rollouts:
            if r.succeeded:
                out.append(Reflection(r.task_id, f"kept: {r.notes or r.task_id}", 0.5))
            else:
                out.append(Reflection(r.task_id, f"gap: {r.notes or r.task_id}", 1.0))
        return out

    @staticmethod
    def aggregate(reflections: Sequence[Reflection]) -> list[Reflection]:
        """Collapse duplicate lessons, summing weight.

        A lesson learned five times is one lesson with more evidence, not five
        lessons — otherwise a repeated failure crowds out every other signal.
        """
        merged: dict[str, Reflection] = {}
        for r in reflections:
            prev = merged.get(r.lesson)
            merged[r.lesson] = (
                Reflection(r.task_id, r.lesson, prev.weight + r.weight) if prev else r
            )
        return sorted(merged.values(), key=lambda r: -r.weight)

    @staticmethod
    def select(reflections: Sequence[Reflection], k: int = 3) -> list[Reflection]:
        """Take the k heaviest lessons. Bounded by construction (S2)."""
        if k <= 0:
            raise ValueError("k must be positive")
        return list(reflections)[:k]

    @staticmethod
    def update(incumbent: str, lessons: Sequence[Reflection]) -> SkillCandidate:
        """Produce a candidate body from the incumbent plus selected lessons."""
        if not lessons:
            return SkillCandidate(incumbent, ())
        added = "\n".join(f"- {l.lesson}" for l in lessons)
        return SkillCandidate(
            f"{incumbent.rstrip()}\n\n## Learned\n{added}\n",
            tuple(l.task_id for l in lessons),
        )

    # -- evaluate ----------------------------------------------------------
    def evaluate(
        self, candidate: SkillCandidate, heldout: Sequence[Rollout]
    ) -> SkillOptResult:
        """Accept ONLY on a strictly positive held-out improvement."""
        n = len(heldout)
        inc = _mean([self._scorer(self.incumbent, r) for r in heldout])
        cand = _mean([self._scorer(candidate.body, r) for r in heldout])

        if n < self._min_heldout:
            return SkillOptResult(
                False,
                f"held-out set too small ({n} < {self._min_heldout}) — a score from "
                "this few samples is noise, and accepting on it is gambling",
                inc, cand, n, self.incumbent,
            )

        if cand > inc:
            return SkillOptResult(
                True, "strictly positive held-out improvement", inc, cand, n,
                candidate.body,
            )

        if cand == inc:
            # The tie case, called out explicitly because it is the one an
            # implementation gets wrong by writing >= and never noticing.
            return SkillOptResult(
                False,
                "tie on held-out score — rejected. Accepting ties lets the skill "
                "drift with no evidence any change helped",
                inc, cand, n, self.incumbent,
            )

        return SkillOptResult(
            False, "candidate scored WORSE on held-out", inc, cand, n, self.incumbent,
        )

    # -- the whole loop ----------------------------------------------------
    def run(
        self, rollouts: Sequence[Rollout], heldout: Sequence[Rollout], k: int = 3
    ) -> SkillOptResult:
        """One full pass. The incumbent is replaced only if `accepted`.

        `heldout` must be disjoint from `rollouts` by task_id — enforced, not
        assumed, because overlap silently turns the held-out score into a
        training score and the loop into a machine for confirming itself.
        """
        train_ids = {r.task_id for r in rollouts}
        overlap = train_ids & {r.task_id for r in heldout}
        if overlap:
            raise ValueError(
                f"held-out set overlaps training rollouts on {sorted(overlap)} — "
                "that scores the candidate on its own homework (S7)"
            )

        candidate = self.update(
            self.incumbent, self.select(self.aggregate(self.reflect(rollouts)), k)
        )
        result = self.evaluate(candidate, heldout)
        if result.accepted:
            self.incumbent = result.skill_body
        self.history.append(result)
        return result
