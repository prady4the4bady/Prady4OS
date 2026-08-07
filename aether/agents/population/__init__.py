"""Population tournament mode (Section 3D #64, DDR-856).

Spec: *"population tournament mode"*.

**Not the same thing as C-02** (`federation/distributed_experiments.py`), which
is checked here because the vocabulary overlaps and DDR-855 was the cost of not
checking. C-02 is *consensus*: registered peers vote, a quorum decides. This is
*competition*: variants are scored head-to-head on the same tasks and ranked by
what they actually won. Voting asks who is preferred; a tournament asks who
performed. They compose — a federation could vote on which tournament to run —
but neither can be implemented in terms of the other.

The rules that make a ranking mean something:

- **A variant that never played cannot win.** Below `MIN_MATCHES` it is
  unranked, not ranked last. "Untested" and "tested and bad" are different
  states, and collapsing them lets a variant win by having avoided scrutiny.
- **A tie does not promote.** Same rule as SkillOpt's acceptance bar (DDR-847):
  promoting on a draw changes the incumbent with no evidence anything improved.
- **The schedule is deterministic** — sorted round-robin. A bracket that depends
  on dict order makes a surprising winner impossible to reproduce.
- **Every match is recorded with both scores**, so a ranking can be audited back
  to the games that produced it rather than taken on trust.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from itertools import combinations

__all__ = [
    "TournamentError",
    "Variant",
    "Match",
    "Standing",
    "Tournament",
    "MIN_MATCHES",
]

# Below this a variant is UNRANKED. Three is the smallest number at which a
# result is not one lucky pairing.
MIN_MATCHES = 3


class TournamentError(ValueError):
    """A tournament operation that must not proceed."""


@dataclass(frozen=True)
class Variant:
    vid: str
    payload: str = ""

    def __post_init__(self) -> None:
        if not self.vid.strip():
            raise TournamentError("variant requires an id")


@dataclass(frozen=True)
class Match:
    """One head-to-head result, with both scores kept.

    Keeping the loser's score matters: a rank built from win counts alone cannot
    distinguish a variant that barely lost every game from one that was crushed,
    and those call for different next moves.
    """

    a: str
    b: str
    score_a: float
    score_b: float
    task_id: str = ""

    @property
    def winner(self) -> str | None:
        """None on a draw — never a coin flip, never index order."""
        if self.score_a > self.score_b:
            return self.a
        if self.score_b > self.score_a:
            return self.b
        return None


@dataclass(frozen=True)
class Standing:
    vid: str
    played: int
    wins: int
    draws: int
    losses: int
    points: float
    ranked: bool
    reason: str = ""


@dataclass
class Tournament:
    """Round-robin over a population, with an auditable standings table."""

    variants: list[Variant] = field(default_factory=list)
    matches: list[Match] = field(default_factory=list)
    min_matches: int = MIN_MATCHES

    def __post_init__(self) -> None:
        if self.min_matches < 1:
            raise TournamentError("min_matches must be at least 1")

    def enter(self, variant: Variant) -> Variant:
        if any(v.vid == variant.vid for v in self.variants):
            raise TournamentError(
                f"{variant.vid!r} is already entered — one variant competing "
                "twice would beat itself and inflate its own record"
            )
        self.variants.append(variant)
        return variant

    def schedule(self) -> list[tuple[str, str]]:
        """Every unordered pairing, in sorted order. Deterministic."""
        ids = sorted(v.vid for v in self.variants)
        if len(ids) < 2:
            raise TournamentError(
                "a tournament needs at least two variants — a population of one "
                "always produces a winner and never produces information"
            )
        return list(combinations(ids, 2))

    def record(
        self, a: str, b: str, score_a: float, score_b: float, task_id: str = ""
    ) -> Match:
        known = {v.vid for v in self.variants}
        for vid in (a, b):
            if vid not in known:
                raise TournamentError(f"{vid!r} is not entered in this tournament")
        if a == b:
            raise TournamentError("a variant cannot play itself")
        match = Match(a, b, float(score_a), float(score_b), task_id)
        self.matches.append(match)
        return match

    # -- standings ---------------------------------------------------------
    def standings(self) -> list[Standing]:
        """Ranked variants first (by points, then id); unranked after.

        Unranked entries are RETURNED, not filtered out, carrying the reason.
        Dropping them would make a variant that never played indistinguishable
        from one that was never entered.
        """
        tally: dict[str, dict[str, float]] = {
            v.vid: {"played": 0, "wins": 0, "draws": 0, "losses": 0, "points": 0.0}
            for v in self.variants
        }
        for m in self.matches:
            for vid in (m.a, m.b):
                tally[vid]["played"] += 1
            w = m.winner
            if w is None:
                tally[m.a]["draws"] += 1
                tally[m.b]["draws"] += 1
                tally[m.a]["points"] += 0.5
                tally[m.b]["points"] += 0.5
            else:
                loser = m.b if w == m.a else m.a
                tally[w]["wins"] += 1
                tally[w]["points"] += 1.0
                tally[loser]["losses"] += 1

        out: list[Standing] = []
        for vid in sorted(tally):
            t = tally[vid]
            ranked = t["played"] >= self.min_matches
            out.append(Standing(
                vid=vid,
                played=int(t["played"]),
                wins=int(t["wins"]),
                draws=int(t["draws"]),
                losses=int(t["losses"]),
                points=t["points"],
                ranked=ranked,
                reason="" if ranked else
                       f"played {int(t['played'])} of {self.min_matches} required "
                       "— unranked, which is not the same as ranked last",
            ))
        out.sort(key=lambda s: (not s.ranked, -s.points, s.vid))
        return out

    def champion(self) -> Standing | None:
        """The outright winner, or None.

        None when the population is untested, when nobody is ranked, or when the
        top two are level. A tie does NOT promote — same rule as SkillOpt's
        acceptance bar: promoting on a draw changes the incumbent with no
        evidence anything improved.
        """
        ranked = [s for s in self.standings() if s.ranked]
        if not ranked:
            return None
        if len(ranked) > 1 and ranked[0].points == ranked[1].points:
            return None
        return ranked[0]
