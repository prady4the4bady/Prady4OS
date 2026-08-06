"""Cost accounting — attribution that refuses to guess (3D #52, DDR-849).

THE DECISION THAT MATTERS: **an unknown model raises. It is never priced at
zero.**

The tempting default is `rates.get(model, 0.0)` — it keeps the code short and
nothing ever crashes. What it actually does is make the cost of every model
nobody has priced yet *invisible*: the total stays plausible, the report looks
complete, and the one number an operator uses to decide whether an agent is
worth running is quietly wrong in the direction that says "keep going". A
missing price is missing information, and missing information must look like
missing information.

That is the same structural defect this project has now hit thirteen times: a
check that absorbs an invalid case instead of rejecting it, so drift is silent
and looks like success.

Costs are integer micro-cents, not floats. Money in binary floating point
accumulates error that shows up as a total which disagrees with the sum of its
own line items — and the disagreement is small enough to be dismissed as a
rounding artefact for a long time.
"""

from __future__ import annotations

from dataclasses import dataclass, field

__all__ = [
    "UnknownModel",
    "ModelRate",
    "CostLedger",
    "MICROCENTS_PER_CENT",
]

MICROCENTS_PER_CENT = 10_000


class UnknownModel(Exception):
    """A model with no published rate. Raised, never priced at zero."""


@dataclass(frozen=True)
class ModelRate:
    """Price per 1000 tokens, in micro-cents.

    Input and output are separate because they are separately priced everywhere
    that sells tokens; collapsing them into one average rate makes a
    prompt-heavy agent and a generation-heavy agent look identical in cost when
    they are not.
    """

    input_per_1k: int
    output_per_1k: int

    def __post_init__(self) -> None:
        if self.input_per_1k < 0 or self.output_per_1k < 0:
            raise ValueError("rates cannot be negative")


@dataclass
class _AgentTotals:
    input_tokens: int = 0
    output_tokens: int = 0
    microcents: int = 0
    calls: int = 0


class CostLedger:
    """Attributes token spend to agents and models. Append-only."""

    def __init__(self, rates: dict[str, ModelRate] | None = None) -> None:
        self._rates: dict[str, ModelRate] = dict(rates or {})
        self._by_agent: dict[str, _AgentTotals] = {}
        self._by_model: dict[str, _AgentTotals] = {}

    def set_rate(self, model: str, rate: ModelRate) -> None:
        if not model.strip():
            raise ValueError("model name must not be empty")
        self._rates[model] = rate

    def price(self, model: str, input_tokens: int, output_tokens: int) -> int:
        """Cost in micro-cents. Raises `UnknownModel` if unpriced.

        Rounds UP. A cost report that rounds down understates spend, and the
        direction of an error in a number used to decide "can we afford to keep
        this running" should never be the reassuring one.
        """
        if input_tokens < 0 or output_tokens < 0:
            raise ValueError("token counts cannot be negative")
        rate = self._rates.get(model)
        if rate is None:
            raise UnknownModel(
                f"no rate published for model {model!r}. Refusing to price it "
                "at zero: an unpriced model charged as free makes the one "
                "number an operator uses to decide whether an agent is worth "
                "running quietly wrong, in the direction that says keep going"
            )
        return (
            (input_tokens * rate.input_per_1k + 999) // 1000
            + (output_tokens * rate.output_per_1k + 999) // 1000
        )

    def record(
        self, agent: str, model: str, input_tokens: int, output_tokens: int
    ) -> int:
        """Attribute one call. Returns its cost in micro-cents.

        Pricing happens FIRST. An unknown model must leave the ledger
        untouched, or a failed pricing call still moves the token totals and
        the ledger's own numbers stop agreeing with each other.
        """
        if not agent.strip():
            raise ValueError("agent must be named — unattributed spend is not spend")
        cost = self.price(model, input_tokens, output_tokens)

        for table, key in ((self._by_agent, agent), (self._by_model, model)):
            totals = table.setdefault(key, _AgentTotals())
            totals.input_tokens += input_tokens
            totals.output_tokens += output_tokens
            totals.microcents += cost
            totals.calls += 1
        return cost

    # -- reporting ---------------------------------------------------------
    @property
    def total_microcents(self) -> int:
        return sum(t.microcents for t in self._by_agent.values())

    def by_agent(self) -> dict[str, _AgentTotals]:
        return dict(self._by_agent)

    def by_model(self) -> dict[str, _AgentTotals]:
        return dict(self._by_model)

    def check_invariant(self) -> None:
        """Per-agent and per-model totals must agree.

        They are two views of the same events, so a disagreement means a
        `record` call updated one and not the other — the kind of bug that
        surfaces as a report nobody can reconcile months later.
        """
        a = sum(t.microcents for t in self._by_agent.values())
        m = sum(t.microcents for t in self._by_model.values())
        if a != m:
            raise AssertionError(
                f"ledger does not reconcile: agents total {a}, models total {m}"
            )

    def format_cents(self) -> str:
        """Total as cents, for display only. Never fed back into arithmetic."""
        return f"{self.total_microcents / MICROCENTS_PER_CENT:.4f}"
