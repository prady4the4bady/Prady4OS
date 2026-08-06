"""Multi-agent skill transfer (Section 3D #49, DDR-848).

When one agent learns something, the other agents should be able to benefit. The
tempting implementation — splice the lesson into the recipient's skill — is
wrong in a way that is hard to see later.

THE DECISION: **a transfer is a proposal, never an application.** A lesson that
improved KRYOS is evidence about KRYOS. It is a hypothesis about PRAX, and the
only thing that can settle it is PRAX's own held-out evaluation. So a transfer
enters the recipient's journal as candidate material and goes through the same
acceptance bar as anything the recipient learned itself.

Applying directly would create a second path into a skill file that bypasses
held-out scoring entirely — and it would be the easy path, so it would become
the path everything took. The loop's guarantees would still be in the code and
would no longer be true of the system.

The second rule: **a lesson naming a capability the recipient lacks is not
transferred.** Teaching an agent a technique it will be denied at the syscall
boundary does not make it more capable; it makes it fail later, further from the
cause, having burned the attempt.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Sequence

from . import Reflection

__all__ = ["TransferProposal", "TransferReport", "propose_transfer"]

_CAP_RE = re.compile(r"\bCAP_[A-Z0-9_]+\b")


@dataclass(frozen=True)
class TransferProposal:
    """A lesson offered from one agent to another, with provenance intact.

    `source_agent` and the originating task_ids are carried rather than dropped:
    a lesson that later turns out to be wrong must be traceable to where it came
    from, and a recipient's history that says only "learned X" cannot answer
    "from whom, and on the strength of what?"
    """

    source_agent: str
    lesson: str
    weight: float
    source_tasks: tuple[str, ...]

    def as_reflection(self) -> Reflection:
        """Recast as recipient-side candidate material.

        The task_id is namespaced by source agent so a transferred lesson can
        never collide with, or masquerade as, a task the recipient actually ran.
        """
        origin = self.source_tasks[0] if self.source_tasks else "unknown"
        return Reflection(f"{self.source_agent}:{origin}", self.lesson, self.weight)


@dataclass(frozen=True)
class TransferReport:
    """What crossed, what did not, and why."""

    accepted: tuple[TransferProposal, ...]
    rejected: tuple[tuple[TransferProposal, str], ...]


def propose_transfer(
    source_agent: str,
    lessons: Sequence[Reflection],
    recipient_caps: frozenset[str] | set[str],
    discount: float = 0.5,
) -> TransferReport:
    """Offer `lessons` to a recipient holding `recipient_caps`.

    Returns proposals for the recipient's journal. **Nothing is applied here** —
    that is the point of the module. The caller feeds the accepted proposals in
    as candidate material and the recipient's own held-out evaluation decides.

    `discount` (default 0.5) halves the weight of a transferred lesson relative
    to one the recipient learned first-hand. Second-hand evidence is weaker
    evidence, and carrying it at full weight lets one agent's experience
    outvote the recipient's own on the recipient's own selection step.
    """
    if not 0.0 < discount <= 1.0:
        raise ValueError(f"discount must be in (0,1], got {discount}")
    if not source_agent.strip():
        raise ValueError("source_agent must be named — transfers carry provenance")

    caps = frozenset(recipient_caps)
    accepted: list[TransferProposal] = []
    rejected: list[tuple[TransferProposal, str]] = []

    for lesson in lessons:
        proposal = TransferProposal(
            source_agent, lesson.lesson, lesson.weight * discount, (lesson.task_id,)
        )
        needed = frozenset(_CAP_RE.findall(lesson.lesson))
        missing = sorted(needed - caps)
        if missing:
            rejected.append((
                proposal,
                f"lesson requires {missing}, which the recipient does not hold — "
                "transferring it would teach a technique the kernel denies, so "
                "the agent fails later and further from the cause",
            ))
            continue
        accepted.append(proposal)

    return TransferReport(tuple(accepted), tuple(rejected))
