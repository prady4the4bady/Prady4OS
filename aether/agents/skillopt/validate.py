"""Skill-update validation (Section 3D #48, DDR-848).

A skill file is a CONTRACT, and SkillOpt rewrites it. That combination is the
whole problem: an agent that revises its own skill prompt is an agent editing
the document that says what it may do. Without a gate, the loop's own
optimisation pressure points at the refusals — they are the lines that make
tasks fail, so removing them raises the score.

So a candidate is validated BEFORE it is ever scored. Not after, and not "the
scorer will notice": a candidate that escalates capability must never reach a
comparison where a good score could carry it through.

The three rules that carry weight:

1. **Capabilities may only shrink.** The declared set must be a subset of what
   the agent was granted. A revision that writes itself a new capability is
   privilege escalation via prose (S1).
2. **Refusals may not be thinned.** The count may rise and may not fall. This is
   the line the optimiser is most motivated to cross, because every refusal is a
   task the agent declines and therefore scores zero on.
3. **The Invariants section is load-bearing and must survive.** An agent that
   edits away its own binding to S1-S8 has removed the text that told it what
   it was bound by.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

__all__ = [
    "SkillValidationError",
    "ValidationReport",
    "parse_capabilities",
    "count_refusals",
    "validate_skill_update",
    "MAX_SKILL_TOKENS",
    "MIN_SKILL_TOKENS",
    "MIN_REFUSALS",
    "UNWIRED_CAPS",
]

# Section 3D #45 budget, same bound test_agent_skills.py enforces on the
# committed roster. A revision that grows past it is no longer the kind of
# document the roster was validated as.
MIN_SKILL_TOKENS = 300
MAX_SKILL_TOKENS = 2000

# DDR-846's floor. Enforced here as "may not FALL BELOW and may not DECREASE",
# which is strictly stronger: an agent starting with 6 refusals cannot quietly
# settle at 3.
MIN_REFUSALS = 3

# Declared in kernel/cap.h, wired to no enforcement path. Naming one without the
# not-yet-spawnable marker is a promise the kernel will refuse at runtime.
UNWIRED_CAPS = ("CAP_EXEC", "CAP_OCR", "CAP_SCENE", "CAP_NET_BROWSE")

REQUIRED_FIELDS = ("**Role:**", "**Capabilities:**", "**Status:**")
REQUIRED_SECTIONS = ("## Refuses", "## Invariants")

_CAP_RE = re.compile(r"\bCAP_[A-Z0-9_]+\b")
_CAP_LINE_RE = re.compile(r"^\s*-\s*\*\*Capabilities:\*\*(.*)$", re.MULTILINE)


class SkillValidationError(Exception):
    """A candidate skill body that must not be scored, let alone adopted."""


@dataclass(frozen=True)
class ValidationReport:
    """Why a candidate passed, in auditable detail.

    Returned on success as well as raised on failure: an operator asking "what
    did the gate actually check?" should not have to read the source.
    """

    capabilities: frozenset[str]
    refusals: int
    tokens: int


def _estimate_tokens(text: str) -> int:
    """Words / 0.75 — the same approximation test_agent_skills.py uses.

    Kept identical on purpose. Two different estimators policing one budget
    means a file can pass one gate and fail the other, and the disagreement
    would look like a flaky test rather than a definition problem.
    """
    return int(len(text.split()) / 0.75)


def parse_capabilities(body: str) -> frozenset[str]:
    """Capabilities DECLARED on the Capabilities line, not merely mentioned.

    Scanning the whole document would be wrong in the dangerous direction: the
    Refuses section legitimately names capabilities in order to decline them
    ("refuses to write to a CAP_SOVEREIGN-locked path"), and a whole-document
    scan would read that as a claim to hold CAP_SOVEREIGN. A gate that
    miscounts refusals as grants punishes exactly the files that are most
    careful.
    """
    m = _CAP_LINE_RE.search(body)
    if m is None:
        raise SkillValidationError(
            "no '- **Capabilities:**' line — a skill that does not state its "
            "capabilities cannot be checked for escalation, so it is refused "
            "rather than assumed empty"
        )
    return frozenset(_CAP_RE.findall(m.group(1)))


def count_refusals(body: str) -> int:
    """Bullet count in the Refuses section."""
    if "## Refuses" not in body:
        raise SkillValidationError("missing '## Refuses' section")
    section = body.split("## Refuses", 1)[1].split("\n## ", 1)[0]
    return sum(1 for ln in section.splitlines() if ln.strip().startswith("-"))


def validate_skill_update(
    incumbent: str, candidate: str, granted: frozenset[str] | set[str] | None = None
) -> ValidationReport:
    """Gate a proposed skill body. Raises `SkillValidationError` if unfit.

    `granted` is the capability set the KERNEL gave this agent. It defaults to
    the incumbent's declared set, which makes the common call site
    (`validate_skill_update(old, new)`) mean "may not gain anything it did not
    already have" — the safe reading, so a caller that forgets the argument
    gets the strict behaviour rather than the permissive one.
    """
    if not candidate.strip():
        raise SkillValidationError("candidate skill body is empty")

    inc_caps = parse_capabilities(incumbent)
    granted_set = frozenset(granted) if granted is not None else inc_caps
    cand_caps = parse_capabilities(candidate)

    # -- 1. capability escalation (S1) ------------------------------------
    gained = cand_caps - granted_set
    if gained:
        raise SkillValidationError(
            f"candidate declares capabilities it was not granted: "
            f"{sorted(gained)}. A skill revision that writes itself a new "
            "capability is privilege escalation through prose (S1)"
        )

    unwired = sorted(c for c in cand_caps if c in UNWIRED_CAPS)
    if unwired:
        low = candidate.lower()
        if "not yet spawnable" not in low and "partially spawnable" not in low:
            raise SkillValidationError(
                f"candidate declares unwired capabilities {unwired} without a "
                "spawnable marker — that is a capability promise the kernel "
                "cannot keep, and the agent will act on it and take -EPERM"
            )

    # -- 2. refusals may not be thinned -----------------------------------
    inc_refusals = count_refusals(incumbent)
    cand_refusals = count_refusals(candidate)
    if cand_refusals < inc_refusals:
        raise SkillValidationError(
            f"candidate drops refusals ({inc_refusals} -> {cand_refusals}). "
            "Every refusal is a task the agent declines and scores zero on, so "
            "this is the exact edit the optimiser is most rewarded for making "
            "and the one it must never be allowed to make"
        )
    if cand_refusals < MIN_REFUSALS:
        raise SkillValidationError(
            f"candidate lists {cand_refusals} refusals, minimum {MIN_REFUSALS}"
        )

    # -- 3. structure survives --------------------------------------------
    for field_name in REQUIRED_FIELDS:
        if field_name not in candidate:
            raise SkillValidationError(f"candidate lost required field {field_name}")
    for section in REQUIRED_SECTIONS:
        if section not in candidate:
            raise SkillValidationError(
                f"candidate lost required section '{section}' — an agent that "
                "edits away its own binding to S1-S8 has deleted the text that "
                "told it what it was bound by"
            )

    # -- 4. budget ---------------------------------------------------------
    tokens = _estimate_tokens(candidate)
    if not MIN_SKILL_TOKENS <= tokens <= MAX_SKILL_TOKENS:
        raise SkillValidationError(
            f"candidate is ~{tokens} tokens, outside the Section 3D #45 budget "
            f"{MIN_SKILL_TOKENS}-{MAX_SKILL_TOKENS}"
        )

    return ValidationReport(cand_caps, cand_refusals, tokens)
