"""D-12 — adaptive goal prioritizer.

Orders the goal queue as circumstances change, and adapts the ordering from
observed outcomes rather than from whatever arrived most recently.

The failure this is built against is **recency capture**. The naive queue is a
stack: whatever was added last is worked on first, because it is the most
salient. That feels responsive and is indistinguishable from responsiveness for
about a day, after which the important-but-old work — the deadline three weeks
out, the blocked dependency, the safety review nobody filed — has been pushed
down by a steady drip of trivia and is never reached. So **recency is one term
among five and cannot by itself put a goal at the front**; the gate pins that
directly.

Score, all terms explicit so the ordering is reproducible (**S9**):

    score = w_value·value
          + w_urgency·urgency(deadline)
          + w_readiness·readiness(blockers)
          + w_recency·recency
          − w_cost·cost

``urgency`` rises as a deadline approaches and saturates; ``readiness`` is zero
while a goal is blocked, because a blocked goal at the front of the queue stalls
everything behind it regardless of how valuable it is.

**Adaptation** (the "adaptive" half): weights move on recorded outcomes, but
only within clamped bounds and only with a minimum sample — a single lucky
success must not rewrite the system's priorities.

Invariants: **S2** (bounded queue, audited eviction), **S9** (deterministic).
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Callable

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "Goal",
    "Weights",
    "ScoredGoal",
    "AdaptiveGoalPrioritizer",
    "PrioritizerError",
    "MAX_GOALS",
    "MIN_ADAPT_SAMPLES",
]

DDR_ID = "D-12"
_FILE = "aether/agents/planner/adaptive_goal_prioritizer.py"
CAPABILITY = "CAP_SUBSTRATE"

DEFAULT_STORE = Path(__file__).resolve().parent / "goal_queue.jsonl"

MAX_GOALS = 4096                 # S2: bounded queue
MIN_ADAPT_SAMPLES = 5            # a lucky run is not a lesson
WEIGHT_MIN = 0.05
WEIGHT_MAX = 2.0
#: Deadlines further out than this contribute no urgency.
URGENCY_HORIZON_S = 7 * 24 * 3600.0


class PrioritizerError(ValueError):
    """Prioritisation refused."""


@dataclass(slots=True)
class Weights:
    value: float = 1.0
    urgency: float = 1.0
    readiness: float = 1.0
    recency: float = 0.2         # deliberately the smallest positive term
    cost: float = 0.5

    def clamped(self) -> Weights:
        def c(x: float) -> float:
            return max(WEIGHT_MIN, min(WEIGHT_MAX, x))
        return Weights(c(self.value), c(self.urgency), c(self.readiness),
                       c(self.recency), c(self.cost))


@dataclass(slots=True)
class Goal:
    goal_id: str
    description: str
    value: float = 0.5              # [0, 1]
    cost: float = 0.0               # [0, 1]
    deadline: float | None = None   # absolute timestamp
    blocked_by: tuple[str, ...] = ()
    created_ts: float = field(default_factory=time.time)
    done: bool = False

    @property
    def blocked(self) -> bool:
        return bool(self.blocked_by)

    def to_json(self) -> str:
        payload = asdict(self)
        payload["blocked_by"] = list(self.blocked_by)
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))


@dataclass(slots=True)
class ScoredGoal:
    goal: Goal
    score: float
    terms: dict[str, float]


class AdaptiveGoalPrioritizer:
    """Scores and orders goals; adapts its weights from outcomes."""

    def __init__(
        self,
        lockbox: Lockbox,
        weights: Weights | None = None,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
        clock: Callable[[], float] | None = None,
        alignment_tap: Callable[[str, dict[str, Any]], None] | None = None,
        max_goals: int = MAX_GOALS,
    ) -> None:
        self.lockbox = lockbox
        self.weights = (weights or Weights()).clamped()
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self.clock = clock if clock is not None else time.time
        self.alignment_tap = alignment_tap
        self.max_goals = int(max_goals)
        self._goals: dict[str, Goal] = {}
        self._outcomes: list[tuple[str, bool]] = []

    def _emit(self, step: str, detail: dict[str, Any]) -> None:
        if self.alignment_tap is not None:
            self.alignment_tap(f"{DDR_ID}.{step}", detail)

    # -- queue ---------------------------------------------------------------
    def add(self, goal: Goal) -> Goal:
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not goal.goal_id:
            raise PrioritizerError("a goal needs an id")
        if not 0.0 <= goal.value <= 1.0:
            raise PrioritizerError(f"value {goal.value} outside [0, 1]")
        if not 0.0 <= goal.cost <= 1.0:
            raise PrioritizerError(f"cost {goal.cost} outside [0, 1]")
        if goal.goal_id in self._goals:
            raise PrioritizerError(f"duplicate goal id: {goal.goal_id}")
        if len(self._goals) >= self.max_goals:
            oldest = min(self._goals.values(), key=lambda g: g.created_ts)
            del self._goals[oldest.goal_id]
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="goal.evicted",
                gate_status="dropped", reason="goal queue full",
                goal_id=oldest.goal_id, window=self.max_goals,
            )
        self._goals[goal.goal_id] = goal
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(goal.to_json() + "\n")
        self._emit("add", {"goal_id": goal.goal_id})
        return goal

    def unblock(self, goal_id: str, blocker: str) -> Goal:
        goal = self._require(goal_id)
        goal.blocked_by = tuple(b for b in goal.blocked_by if b != blocker)
        return goal

    def complete(self, goal_id: str, success: bool = True) -> Goal:
        goal = self._require(goal_id)
        goal.done = True
        self._outcomes.append((goal_id, success))
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="goal.complete",
            gate_status="success" if success else "failure", goal_id=goal_id,
        )
        return goal

    def _require(self, goal_id: str) -> Goal:
        goal = self._goals.get(goal_id)
        if goal is None:
            raise PrioritizerError(f"unknown goal: {goal_id}")
        return goal

    # -- scoring -------------------------------------------------------------
    def _urgency(self, goal: Goal, now: float) -> float:
        """1.0 at or past the deadline, 0.0 beyond the horizon."""
        if goal.deadline is None:
            return 0.0
        remaining = goal.deadline - now
        if remaining <= 0.0:
            return 1.0
        if remaining >= URGENCY_HORIZON_S:
            return 0.0
        return 1.0 - (remaining / URGENCY_HORIZON_S)

    def _recency(self, goal: Goal, now: float) -> float:
        """Newer goals score slightly higher — slightly being the point."""
        age = max(0.0, now - goal.created_ts)
        if age >= URGENCY_HORIZON_S:
            return 0.0
        return 1.0 - (age / URGENCY_HORIZON_S)

    def score(self, goal: Goal, now: float | None = None) -> ScoredGoal:
        """Score one goal, returning every term so the ordering is auditable."""
        moment = self.clock() if now is None else now
        w = self.weights
        terms = {
            "value": w.value * goal.value,
            "urgency": w.urgency * self._urgency(goal, moment),
            # A blocked goal at the front stalls everything behind it, however
            # valuable it is.
            "readiness": 0.0 if goal.blocked else w.readiness,
            "recency": w.recency * self._recency(goal, moment),
            "cost": -w.cost * goal.cost,
        }
        return ScoredGoal(goal=goal, score=sum(terms.values()), terms=terms)

    def prioritize(self, include_done: bool = False) -> list[ScoredGoal]:
        """The ordered queue, highest score first."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        now = self.clock()
        pending = [g for g in self._goals.values() if include_done or not g.done]
        scored = [self.score(g, now) for g in pending]
        # goal_id breaks exact ties so the order never depends on dict insertion.
        scored.sort(key=lambda s: (-s.score, s.goal.goal_id))
        self._emit("prioritize", {"goals": len(scored)})
        return scored

    def next_goal(self) -> Goal | None:
        ordered = self.prioritize()
        return ordered[0].goal if ordered else None

    # -- adaptation ----------------------------------------------------------
    def adapt(self, step: float = 0.1) -> Weights:
        """Move the weights on the recorded outcome history.

        Requires ``MIN_ADAPT_SAMPLES`` outcomes: a single lucky success must not
        rewrite the system's priorities. Weights stay clamped, so no term can
        run away and collapse the ordering onto itself.
        """
        self.lockbox.require(DDR_ID, CAPABILITY)
        if len(self._outcomes) < MIN_ADAPT_SAMPLES:
            raise PrioritizerError(
                f"{len(self._outcomes)} outcomes below the adaptation floor "
                f"{MIN_ADAPT_SAMPLES}"
            )
        successes = sum(1 for _gid, ok in self._outcomes if ok)
        rate = successes / len(self._outcomes)
        # Doing badly => lean harder on readiness and cost (pick work that can
        # actually be finished); doing well => lean back toward raw value.
        direction = 1.0 if rate < 0.5 else -1.0
        proposed = Weights(
            value=self.weights.value - direction * step,
            urgency=self.weights.urgency,
            readiness=self.weights.readiness + direction * step,
            recency=self.weights.recency,
            cost=self.weights.cost + direction * step,
        )
        self.weights = proposed.clamped()
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="goal.adapt", gate_status="adapted",
            success_rate=round(rate, 4), weights=asdict(self.weights),
        )
        self._emit("adapt", {"success_rate": rate})
        return self.weights

    @property
    def goals(self) -> tuple[Goal, ...]:
        return tuple(sorted(self._goals.values(), key=lambda g: g.goal_id))
