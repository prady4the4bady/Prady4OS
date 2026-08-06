"""`goals.md` per agent (Section 3D #53, DDR-851).

A goals file is the agent's objective function in prose. That makes it the same
class of document as `skill.md` and it inherits the same protection: revising it
requires `CAP_SOVEREIGN`, because an agent that can edit what it is trying to
achieve has no objective function at all — it has a preference it can update
whenever the current one proves inconvenient.

THE RULE THAT DOES THE WORK: **a goal with no checkable success criterion is
refused.** "Improve file handling" cannot be satisfied or failed; it can only be
asserted about. Once a goal is unfalsifiable, every subsequent report against it
is unfalsifiable too, and the whole tracking apparatus produces confident
statements that carry no information. A criterion does not have to be numeric —
it has to be something a third party could check without asking the agent.

SECOND RULE: **an agent cannot mark its own goal complete.** Completion records
who attested it. This is S4 in the same shape it takes everywhere else in this
system: the agent proposes, something else confirms.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from enum import Enum

__all__ = [
    "GoalError",
    "GoalStatus",
    "Goal",
    "GoalSet",
    "parse_goals",
    "CAP_SOVEREIGN",
    "MAX_ACTIVE_GOALS",
]

CAP_SOVEREIGN = "CAP_SOVEREIGN"

# An agent pursuing thirty goals is pursuing none of them. The cap is a forcing
# function for prioritisation, not a storage limit (S2).
MAX_ACTIVE_GOALS = 8

_GOAL_RE = re.compile(
    r"^-\s*\[(?P<state>[ x~])\]\s*"
    r"\((?P<gid>[A-Za-z0-9_.-]+)\)\s*"
    r"(?P<statement>.+?)"
    r"\s*\|\s*success:\s*(?P<criterion>.+?)\s*$"
)


class GoalStatus(Enum):
    OPEN = "open"
    IN_PROGRESS = "in_progress"
    COMPLETE = "complete"


_STATE_MAP = {" ": GoalStatus.OPEN, "~": GoalStatus.IN_PROGRESS, "x": GoalStatus.COMPLETE}


class GoalError(Exception):
    """A goals document that must not be adopted."""


@dataclass
class Goal:
    gid: str
    statement: str
    criterion: str
    status: GoalStatus = GoalStatus.OPEN
    priority: int = 0
    completed_by: str | None = None

    def __post_init__(self) -> None:
        if not self.gid.strip():
            raise GoalError("goal id must not be empty")
        if not self.statement.strip():
            raise GoalError(f"goal {self.gid}: empty statement")
        if not self.criterion.strip():
            raise GoalError(
                f"goal {self.gid}: no success criterion. A goal that cannot be "
                "checked cannot be satisfied or failed — only asserted about, "
                "and every report against it is then unfalsifiable too"
            )

    def complete(self, attestor: str) -> None:
        """Mark complete. `attestor` is REQUIRED and may not be the agent.

        The caller passes who confirmed it. An agent marking its own goal done
        is the self-approval failure (S4) in the one place where it would be
        least visible: the record would look exactly like a real completion.
        """
        if not attestor.strip():
            raise GoalError(
                f"goal {self.gid}: completion requires an attestor. An agent "
                "cannot mark its own goal complete (S4)"
            )
        self.status = GoalStatus.COMPLETE
        self.completed_by = attestor


@dataclass
class GoalSet:
    """An agent's goals, ordered by descending priority."""

    agent: str
    goals: list[Goal] = field(default_factory=list)

    @property
    def active(self) -> list[Goal]:
        return [g for g in self.goals if g.status is not GoalStatus.COMPLETE]

    def by_priority(self) -> list[Goal]:
        return sorted(self.goals, key=lambda g: (-g.priority, g.gid))

    def get(self, gid: str) -> Goal:
        for g in self.goals:
            if g.gid == gid:
                return g
        raise KeyError(gid)

    def diff_against(self, achieved: set[str]) -> list[Goal]:
        """Goals not in `achieved` — the input to the subconscious loop.

        Returned in priority order so a caller that takes the first N takes the
        N that matter, rather than the N that happened to be written first.
        """
        return [g for g in self.by_priority() if g.gid not in achieved
                and g.status is not GoalStatus.COMPLETE]


def parse_goals(
    agent: str, text: str, approver_caps: frozenset[str] | set[str] | None = None
) -> GoalSet:
    """Parse and validate a `goals.md` body.

    Requires `CAP_SOVEREIGN`, for the same reason `skill.md` does: this is the
    objective function. An agent that can rewrite it can satisfy any goal by
    replacing it with one it has already met.

    Expected line format:

        - [ ] (gid) statement | success: criterion

    `[ ]` open, `[~]` in progress, `[x]` complete. Priority comes from a
    `priority: N` trailer on the preceding `## ` heading, defaulting to 0.
    """
    caps = frozenset(approver_caps) if approver_caps is not None else frozenset()
    if CAP_SOVEREIGN not in caps:
        raise GoalError(
            "goals.md requires CAP_SOVEREIGN. It is the objective function: an "
            "agent that can edit what it is trying to achieve does not have an "
            "objective, it has a preference it can update when the current one "
            "proves inconvenient"
        )
    if not agent.strip():
        raise GoalError("goals must belong to a named agent")

    goals: list[Goal] = []
    priority = 0
    seen: set[str] = set()

    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if line.startswith("##"):
            m = re.search(r"priority:\s*(-?\d+)", line)
            priority = int(m.group(1)) if m else 0
            continue
        if not line.startswith("- ["):
            continue

        m = _GOAL_RE.match(line)
        if m is None:
            # A malformed goal line is REJECTED, not skipped. Skipping means a
            # typo silently removes a goal, and the agent then reports full
            # completion of a list that is quietly shorter than the operator's.
            raise GoalError(
                f"line {lineno}: malformed goal line {line!r}. Expected "
                "'- [ ] (gid) statement | success: criterion'. Refusing to skip "
                "it: a skipped line is a goal that silently stopped existing"
            )

        gid = m.group("gid")
        if gid in seen:
            raise GoalError(f"line {lineno}: duplicate goal id {gid!r}")
        seen.add(gid)
        goals.append(
            Goal(
                gid=gid,
                statement=m.group("statement").strip(),
                criterion=m.group("criterion").strip(),
                status=_STATE_MAP[m.group("state")],
                priority=priority,
            )
        )

    if not goals:
        raise GoalError(
            "no goals parsed. An empty goals.md is almost always a format error "
            "rather than an agent with nothing to do, and treating it as the "
            "latter makes the mistake invisible"
        )

    active = [g for g in goals if g.status is not GoalStatus.COMPLETE]
    if len(active) > MAX_ACTIVE_GOALS:
        raise GoalError(
            f"{len(active)} active goals, maximum {MAX_ACTIVE_GOALS}. An agent "
            "pursuing this many is pursuing none of them"
        )
    return GoalSet(agent, goals)
