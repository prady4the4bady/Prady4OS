"""Subconscious background loop (Section 3D #54, DDR-851).

Spec: *"wakes every N ticks, goal-diff prompts, bounded by the 60 syscall/s
limiter"*, and F#71: *"fires proactive prompts to **idle** agents"*.

Every clause there is a constraint, and each one is load-bearing:

- **Wakes every N ticks** — not continuously. A background loop with no period
  is a busy loop competing with the foreground work it exists to support.
- **Goal-diff** — it proposes based on the gap between goals and what has been
  achieved, so it has a reason for every prompt it emits. A background loop that
  generates prompts without a diff is generating busywork with a schedule.
- **Bounded by the 60 syscall/s limiter** — the kernel's rate limit is a
  ceiling the loop must *stay under by construction*, not discover by being
  throttled. A background task that consumes the syscall budget starves the
  foreground agent, and the symptom is that the agent gets slow, not that the
  subconscious looks wrong.
- **Idle agents only** — prompting a busy agent interrupts the task it is
  already doing, on the strength of a goal it may already be pursuing.

THE THING THIS MODULE MUST NOT DO: act. It emits prompts. Every one goes through
the ordinary approval path like any other action, because a background loop with
an execution path is an unsupervised agent that runs on a timer.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from . import Goal, GoalSet

__all__ = [
    "Prompt",
    "SubconsciousLoop",
    "RateLimitExceeded",
    "SYSCALL_LIMIT_PER_S",
    "DEFAULT_TICK_PERIOD",
    "SUBCONSCIOUS_SYSCALL_SHARE",
]

# The kernel's per-agent limiter (DDR-707 lineage). Mirrored here so the loop
# can stay under it by construction rather than by being throttled.
SYSCALL_LIMIT_PER_S = 60

# The subconscious gets a MINORITY share. It is background work; the foreground
# agent must retain the majority of its own syscall budget, or the loop meant to
# help the agent becomes the reason the agent is slow.
SUBCONSCIOUS_SYSCALL_SHARE = 0.25

DEFAULT_TICK_PERIOD = 1000  # ticks between wakes


class RateLimitExceeded(Exception):
    """The loop would exceed its share of the syscall budget."""


@dataclass(frozen=True)
class Prompt:
    """A proposal, carrying the goal that justifies it.

    `goal_id` is not decoration. A prompt with no goal behind it cannot be
    reviewed — an operator asking "why is it suggesting this?" would have
    nothing to read.
    """

    agent: str
    goal_id: str
    text: str
    tick: int


@dataclass
class SubconsciousLoop:
    """Wakes on a period, diffs goals, emits prompts to idle agents."""

    period: int = DEFAULT_TICK_PERIOD
    max_prompts_per_wake: int = 3
    syscalls_per_prompt: int = 4
    _last_wake: int = -1
    _window_start: int = 0
    _syscalls_this_window: int = 0
    emitted: list[Prompt] = field(default_factory=list)
    skipped_busy: list[str] = field(default_factory=list)

    def __post_init__(self) -> None:
        if self.period <= 0:
            raise ValueError("period must be positive — a loop with no period "
                             "is a busy loop competing with the work it supports")
        if self.max_prompts_per_wake <= 0:
            raise ValueError("max_prompts_per_wake must be positive")
        if self.syscalls_per_prompt <= 0:
            raise ValueError("syscalls_per_prompt must be positive")

    @property
    def budget_per_second(self) -> int:
        """The loop's own ceiling: a minority share of the kernel limiter."""
        return int(SYSCALL_LIMIT_PER_S * SUBCONSCIOUS_SYSCALL_SHARE)

    def due(self, tick: int) -> bool:
        """True when `period` ticks have elapsed since the last wake."""
        if tick < 0:
            raise ValueError("tick must not be negative")
        return self._last_wake < 0 or tick - self._last_wake >= self.period

    def wake(
        self,
        tick: int,
        goals: GoalSet,
        achieved: set[str],
        idle_agents: set[str],
        ticks_per_second: int = 1000,
    ) -> list[Prompt]:
        """One wake. Returns prompts — it never executes anything.

        Raises `RateLimitExceeded` rather than emitting a truncated batch: a
        partial batch silently drops the lowest-priority goals, and the loop
        then looks like it is keeping up with a goal list it is not reading to
        the end of.
        """
        if not self.due(tick):
            return []
        if goals.agent not in idle_agents:
            # Prompting a busy agent interrupts work already in progress, on the
            # strength of a goal it may already be pursuing.
            self.skipped_busy.append(goals.agent)
            self._last_wake = tick
            return []

        pending: list[Goal] = goals.diff_against(achieved)[: self.max_prompts_per_wake]
        needed = len(pending) * self.syscalls_per_prompt

        # Slide the 1-second accounting window.
        if tick - self._window_start >= ticks_per_second:
            self._window_start = tick
            self._syscalls_this_window = 0

        if self._syscalls_this_window + needed > self.budget_per_second:
            raise RateLimitExceeded(
                f"wake would use {needed} syscalls on top of "
                f"{self._syscalls_this_window} already spent this second, over "
                f"the loop's {self.budget_per_second}/s share of the "
                f"{SYSCALL_LIMIT_PER_S}/s kernel limit. Refusing rather than "
                "emitting a partial batch — a truncated batch silently drops "
                "the lowest-priority goals while looking like it kept up"
            )

        self._syscalls_this_window += needed
        self._last_wake = tick

        prompts = [
            Prompt(
                agent=goals.agent,
                goal_id=g.gid,
                text=(
                    f"Goal {g.gid} is not yet met: {g.statement}. "
                    f"Success is: {g.criterion}."
                ),
                tick=tick,
            )
            for g in pending
        ]
        self.emitted.extend(prompts)
        return prompts
