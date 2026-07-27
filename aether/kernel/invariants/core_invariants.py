"""Section E — S1..S14, the invariant system for the AETHER agent layer.

These are the authoritative invariants for THIS layer. They are deliberately
**independent** of the C kernel's S1..S8 (docs/AETHER_MASTER_FEATURES.md
Section H, which governs the bare-metal kernel) and the two sets must never be
merged — the numbering collides but the meanings do not.

``InvariantEnforcer`` (C-07) loads these plus any custom invariants compiled
from natural language and is the single ``check_action`` gate.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Mapping

__all__ = ["Invariant", "CORE_INVARIANTS", "get_invariant", "invariant_ids"]


@dataclass(slots=True, frozen=True)
class Invariant:
    """One binding rule."""

    ident: str
    statement: str
    severity: int          # 1..5; >= 4 requires Offline Safety Review (C-08)
    auto_block: bool


CORE_INVARIANTS: tuple[Invariant, ...] = (
    Invariant("S1", "Never execute unsigned code in SOVEREIGN context.", 5, True),
    Invariant("S2", "Never exfiltrate user data without explicit consent.", 5, True),
    Invariant("S3", "Never bypass Offline Safety Review for risk_level >= 3.", 5, True),
    Invariant("S4", "Never forge or alter audit log entries.", 5, True),
    Invariant("S5", "Never impersonate another agent's identity on the mesh.", 4, True),
    Invariant("S6", "Never acquire capabilities beyond declared contract scope.", 5, True),
    Invariant("S7", "Never disable or circumvent the prompt-injection firewall.", 5, True),
    Invariant("S8", "Never make irreversible filesystem changes without dry-run.", 4, True),
    Invariant("S9", "Never accept a knowledge packet without Merkle verification.", 4, True),
    Invariant(
        "S10",
        "Never promote an improvement proposal without human review if risk_level >= 3.",
        4,
        True,
    ),
    Invariant("S11", "Never run an agent without a registered, verified contract.", 4, True),
    Invariant("S12", "Never suppress a Superalignment Monitor alert.", 5, True),
    Invariant(
        "S13",
        "Never execute a multi-step plan without at least one counterfactual "
        "simulation for the highest-risk step.",
        3,
        False,
    ),
    Invariant(
        "S14",
        "Never make a decision with overall ethical score < 0.4 without first "
        "triggering EthicalVetoError and Offline Review.",
        5,
        True,
    ),
)

_BY_ID: Mapping[str, Invariant] = {inv.ident: inv for inv in CORE_INVARIANTS}


def get_invariant(ident: str) -> Invariant:
    inv = _BY_ID.get(ident)
    if inv is None:
        raise KeyError(f"unknown invariant: {ident}")
    return inv


def invariant_ids() -> tuple[str, ...]:
    return tuple(inv.ident for inv in CORE_INVARIANTS)
