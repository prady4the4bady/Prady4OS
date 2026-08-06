"""TokenJuice — token budgets that refuse rather than degrade (3D #50, DDR-849).

An agent with no budget runs until something else stops it. An agent with a
*soft* budget is worse: it keeps going and quietly does less per step, so the
run "succeeds" having answered from a truncated context, and the only evidence
is that the output got vaguer.

So: **a budget is a hard ceiling. When it is exhausted, the request is REFUSED,
not shrunk.** A refusal is a fact an operator can act on. A silently truncated
context is a wrong answer wearing a right answer's shape.

Three properties follow, and each exists because of a specific way this goes
wrong:

- **Reserve.** A budget spent to exactly zero leaves an agent unable to finish —
  no summary, no cleanup, no audit entry explaining why it stopped. `reserve`
  is headroom that ordinary work cannot touch and completion can.
- **Sub-budgets cannot exceed their parent.** An agent that can hand its child
  more than it holds has escalated by delegation (S1). The parent's grant is
  debited at the moment it is made, not when the child spends it.
- **Reservation is two-phase.** `reserve_tokens()` then `commit()` or
  `release()`. Charging after the fact allows an overspend to have already
  happened, and "we noticed afterwards" is not a ceiling.
"""

from __future__ import annotations

from dataclasses import dataclass

__all__ = [
    "BudgetExhausted",
    "Reservation",
    "TokenBudget",
    "DEFAULT_RESERVE_FRACTION",
]

# Fraction of a budget fenced off for completion work. 5% — large enough to
# write a final summary and an audit entry, small enough that it is not a
# meaningful tax on the actual work.
DEFAULT_RESERVE_FRACTION = 0.05


class BudgetExhausted(Exception):
    """A request that would exceed the ceiling. Raised, never rounded down."""


@dataclass(frozen=True)
class Reservation:
    """An outstanding claim on a budget: held, not yet spent."""

    tokens: int
    label: str


class TokenBudget:
    """A hard token ceiling with reserved completion headroom.

    Accounting identity, true after every operation:

        total == spent + outstanding + available + reserve_remaining
    """

    def __init__(
        self,
        total: int,
        label: str = "root",
        reserve_fraction: float = DEFAULT_RESERVE_FRACTION,
    ) -> None:
        if total <= 0:
            raise ValueError(f"budget must be positive, got {total}")
        if not 0.0 <= reserve_fraction < 1.0:
            raise ValueError(f"reserve_fraction must be in [0,1), got {reserve_fraction}")
        self.label = label
        self.total = total
        self._reserve = int(total * reserve_fraction)
        # Normal and completion pools are tracked SEPARATELY. Sharing one
        # counter double-counts a completion reservation — once against the
        # fenced reserve and once against ordinary availability — and the books
        # stop balancing while every individual operation still looks right.
        self._spent_normal = 0
        self._spent_reserve = 0
        self._out_normal = 0
        self._out_reserve = 0
        self._granted = 0
        self._reservations: dict[int, tuple[Reservation, bool]] = {}
        self._next_id = 1

    # -- accounting --------------------------------------------------------
    @property
    def spent(self) -> int:
        return self._spent_normal + self._spent_reserve

    @property
    def outstanding(self) -> int:
        return self._out_normal + self._out_reserve

    @property
    def granted(self) -> int:
        """Debited to children. Gone from this budget whether spent or not."""
        return self._granted

    @property
    def reserve_remaining(self) -> int:
        return self._reserve - self._spent_reserve - self._out_reserve

    @property
    def available(self) -> int:
        """Spendable by ORDINARY work — excludes the completion reserve.

        Deliberately NOT clamped at zero. Clamping would hide a negative, which
        is precisely the state that means the ceiling has already been breached.
        """
        return (
            self.total
            - self._reserve
            - self._spent_normal
            - self._out_normal
            - self._granted
        )

    def check_invariant(self) -> None:
        """Assert the conditions that can actually be violated.

        NOTE ON WHAT THIS DELIBERATELY DOES NOT CHECK: the identity
        `total == spent + outstanding + granted + available + reserve_remaining`
        is a TAUTOLOGY here, because `available` is derived by subtracting the
        others from `total`. Asserting it would look like a strong check and
        could never fail — a check that cannot fail is indistinguishable from
        no check, which is this project's recurring defect wearing a
        reassuring costume. So the identity is documented and the assertions
        below test the parts that are independently tracked.
        """
        problems = []
        if self.available < 0:
            problems.append(f"available={self.available} is negative — the "
                            "ordinary-budget ceiling has been breached")
        if self.reserve_remaining < 0:
            problems.append(f"reserve_remaining={self.reserve_remaining} is "
                            "negative — completion headroom overspent")
        if self._out_normal < 0 or self._out_reserve < 0:
            problems.append(f"outstanding went negative "
                            f"(normal={self._out_normal}, reserve={self._out_reserve}) "
                            "— a reservation was released twice")
        if self._spent_normal < 0 or self._spent_reserve < 0:
            problems.append("spend went negative")
        held = sum(r.tokens for r, c in self._reservations.values() if not c)
        if held != self._out_normal:
            problems.append(f"outstanding normal counter ({self._out_normal}) "
                            f"disagrees with the open reservations ({held})")
        held_r = sum(r.tokens for r, c in self._reservations.values() if c)
        if held_r != self._out_reserve:
            problems.append(f"outstanding reserve counter ({self._out_reserve}) "
                            f"disagrees with the open reservations ({held_r})")
        if problems:
            raise AssertionError(
                f"budget {self.label!r} is inconsistent: " + "; ".join(problems)
            )

    # -- two-phase spend ---------------------------------------------------
    def reserve_tokens(self, tokens: int, label: str = "", completion: bool = False) -> int:
        """Claim `tokens`. Returns a reservation id. Raises if unaffordable.

        `completion=True` draws on the fenced reserve — for the final summary,
        the cleanup, the audit entry that says why the run stopped. Ordinary
        work can never reach it, which is the entire point of fencing it.
        """
        if tokens <= 0:
            raise ValueError(f"tokens must be positive, got {tokens}")

        pool = self.reserve_remaining if completion else self.available
        if tokens > pool:
            kind = "completion reserve" if completion else "budget"
            raise BudgetExhausted(
                f"{self.label!r}: request for {tokens} exceeds {kind} "
                f"({pool} left). Refused rather than truncated — a silently "
                "shortened context produces a confident answer from less "
                "information than the caller thinks it had"
            )

        rid = self._next_id
        self._next_id += 1
        self._reservations[rid] = (Reservation(tokens, label), completion)
        if completion:
            self._out_reserve += tokens
        else:
            self._out_normal += tokens
        return rid

    def commit(self, rid: int, actual: int | None = None) -> int:
        """Convert a reservation to spend. `actual` may be less, never more.

        Unused headroom returns to the pool it came from — a completion
        reservation that came in under does not leak into ordinary spend.
        """
        entry = self._reservations.pop(rid, None)
        if entry is None:
            raise KeyError(f"no outstanding reservation {rid}")
        res, completion = entry
        used = res.tokens if actual is None else actual
        if used < 0:
            raise ValueError("actual usage cannot be negative")
        if used > res.tokens:
            # Put it back before raising: a failed commit must not silently
            # consume the reservation and leave the books short.
            self._reservations[rid] = entry
            raise BudgetExhausted(
                f"{self.label!r}: reservation {rid} was for {res.tokens} tokens "
                f"but {used} were used. The overspend has already happened; "
                "reserve the true amount up front"
            )
        if completion:
            self._out_reserve -= res.tokens
            self._spent_reserve += used
        else:
            self._out_normal -= res.tokens
            self._spent_normal += used
        return used

    def release(self, rid: int) -> None:
        """Abandon a reservation, spending nothing."""
        entry = self._reservations.pop(rid, None)
        if entry is None:
            raise KeyError(f"no outstanding reservation {rid}")
        res, completion = entry
        if completion:
            self._out_reserve -= res.tokens
        else:
            self._out_normal -= res.tokens

    # -- delegation --------------------------------------------------------
    def grant(self, tokens: int, label: str) -> "TokenBudget":
        """Carve a child budget out of this one.

        Debited NOW, not when the child spends. A parent whose available figure
        still counts tokens it has already promised away will authorise work it
        cannot pay for, and the shortfall surfaces at the child, far from the
        decision that caused it.
        """
        if tokens <= 0:
            raise ValueError(f"grant must be positive, got {tokens}")
        if tokens > self.available:
            raise BudgetExhausted(
                f"{self.label!r}: cannot grant {tokens} to {label!r}, only "
                f"{self.available} available. An agent that hands a child more "
                "than it holds has escalated by delegation (S1)"
            )
        self._granted += tokens
        return TokenBudget(tokens, label)
