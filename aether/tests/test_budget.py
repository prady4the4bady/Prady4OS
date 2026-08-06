"""TokenJuice, trajectory and cost accounting (3D #50-52, DDR-849)."""
from __future__ import annotations

import io
import json

import pytest

from aether.agents.budget import BudgetExhausted, TokenBudget
from aether.agents.budget.cost import (
    CostLedger,
    ModelRate,
    UnknownModel,
)
from aether.agents.budget.trajectory import (
    MAX_FIELD_CHARS,
    REDACTED,
    TrajectoryWriter,
    read_trajectory,
    redact,
)

# ==========================================================================
# #50 TokenJuice — a ceiling that refuses
# ==========================================================================
def test_request_over_budget_is_refused_not_truncated() -> None:
    """THE decision. A shortened context is a wrong answer's best disguise."""
    b = TokenBudget(1000, reserve_fraction=0.0)
    with pytest.raises(BudgetExhausted, match="Refused rather than truncated"):
        b.reserve_tokens(1001)
    assert b.spent == 0


def test_books_balance_through_a_full_lifecycle() -> None:
    b = TokenBudget(10_000)
    b.check_invariant()
    r1 = b.reserve_tokens(1000, "a")
    b.check_invariant()
    b.commit(r1, 600)
    b.check_invariant()
    r2 = b.reserve_tokens(500, "b")
    b.release(r2)
    b.check_invariant()
    child = b.grant(2000, "child")
    b.check_invariant()
    assert b.spent == 600 and b.outstanding == 0 and b.granted == 2000
    assert child.total == 2000


def test_completion_reserve_is_not_double_counted() -> None:
    """The bug this module was written with and caught before shipping.

    A completion reservation must debit the reserve ONLY. Charging it to
    ordinary availability as well unbalances the books while every individual
    operation still looks correct.
    """
    b = TokenBudget(10_000, reserve_fraction=0.1)  # reserve = 1000
    before = b.available
    rid = b.reserve_tokens(500, "final summary", completion=True)
    b.check_invariant()
    assert b.available == before, "completion work must not consume ordinary budget"
    assert b.reserve_remaining == 500
    b.commit(rid)
    b.check_invariant()
    assert b.available == before


def test_ordinary_work_can_never_reach_the_reserve() -> None:
    """The reserve exists so an agent can always explain why it stopped."""
    b = TokenBudget(1000, reserve_fraction=0.1)  # 100 reserved, 900 usable
    b.commit(b.reserve_tokens(900))
    assert b.available == 0
    with pytest.raises(BudgetExhausted):
        b.reserve_tokens(1)
    # ...but completion work still can.
    b.commit(b.reserve_tokens(100, "summary", completion=True))
    b.check_invariant()


def test_completion_request_over_reserve_is_refused() -> None:
    b = TokenBudget(1000, reserve_fraction=0.1)
    with pytest.raises(BudgetExhausted, match="completion reserve"):
        b.reserve_tokens(101, completion=True)


def test_commit_over_reservation_raises_and_leaves_books_intact() -> None:
    """The overspend already happened; the ledger must not also be corrupted."""
    b = TokenBudget(10_000)
    rid = b.reserve_tokens(100)
    with pytest.raises(BudgetExhausted, match="overspend"):
        b.commit(rid, 150)
    b.check_invariant()
    assert b.outstanding == 100, "a failed commit must not consume the reservation"
    b.commit(rid, 100)
    b.check_invariant()


def test_unused_reservation_returns_to_its_own_pool() -> None:
    b = TokenBudget(10_000, reserve_fraction=0.1)
    rid = b.reserve_tokens(500, completion=True)
    b.commit(rid, 100)
    b.check_invariant()
    assert b.reserve_remaining == 900, "unused completion headroom returns to reserve"


def test_grant_is_debited_immediately_not_on_child_spend() -> None:
    """A parent counting tokens it already promised away authorises work it
    cannot pay for, and the shortfall surfaces at the child."""
    b = TokenBudget(1000, reserve_fraction=0.0)
    b.grant(600, "child")
    assert b.available == 400
    with pytest.raises(BudgetExhausted, match="escalated by delegation"):
        b.grant(500, "child2")


def test_child_cannot_exceed_parent() -> None:
    b = TokenBudget(1000, reserve_fraction=0.0)
    with pytest.raises(BudgetExhausted):
        b.grant(1001, "greedy")


def test_available_is_not_clamped_so_a_breach_stays_visible() -> None:
    b = TokenBudget(1000, reserve_fraction=0.0)
    b.commit(b.reserve_tokens(1000))
    assert b.available == 0
    b._granted += 50  # simulate a bug elsewhere
    assert b.available == -50, "clamping would hide exactly the breached state"
    with pytest.raises(AssertionError):
        b.check_invariant()


def test_invariant_catches_a_counter_desynced_from_its_reservations() -> None:
    """The check must have teeth beyond the derived identity.

    `available` is computed by subtraction, so an accounting identity over it
    is a tautology that can never fail. These are the independently-tracked
    quantities, which is where a real bug would show.
    """
    b = TokenBudget(10_000)
    b.reserve_tokens(100)
    b.check_invariant()
    b._out_normal += 25  # a lost decrement somewhere
    with pytest.raises(AssertionError, match="disagrees with the open reservations"):
        b.check_invariant()


def test_invariant_catches_an_overspent_reserve() -> None:
    b = TokenBudget(1000, reserve_fraction=0.1)
    b._spent_reserve = 150  # more than the 100 fenced off
    with pytest.raises(AssertionError, match="completion headroom overspent"):
        b.check_invariant()


def test_rejects_nonsense_construction() -> None:
    for bad in (0, -1):
        with pytest.raises(ValueError):
            TokenBudget(bad)
    with pytest.raises(ValueError):
        TokenBudget(100, reserve_fraction=1.0)
    with pytest.raises(ValueError):
        TokenBudget(100).reserve_tokens(0)
    with pytest.raises(ValueError):
        TokenBudget(100).grant(0, "x")


def test_unknown_reservation_id_raises() -> None:
    b = TokenBudget(100)
    with pytest.raises(KeyError):
        b.commit(999)
    with pytest.raises(KeyError):
        b.release(999)


# ==========================================================================
# #52 cost — an unknown model is never free
# ==========================================================================
RATES = {
    "local-7b": ModelRate(0, 0),
    "big": ModelRate(300, 1500),
}


def test_unknown_model_raises_instead_of_pricing_at_zero() -> None:
    """THE decision. `rates.get(model, 0.0)` is the bug this test exists for."""
    ledger = CostLedger(RATES)
    with pytest.raises(UnknownModel, match="Refusing to price it"):
        ledger.price("brand-new-model", 1000, 1000)


def test_unknown_model_leaves_the_ledger_untouched() -> None:
    """A failed pricing call must not still move the token totals."""
    ledger = CostLedger(RATES)
    ledger.record("KRYOS", "big", 1000, 1000)
    before = ledger.total_microcents
    with pytest.raises(UnknownModel):
        ledger.record("KRYOS", "unpriced", 5000, 5000)
    assert ledger.total_microcents == before
    assert ledger.by_agent()["KRYOS"].input_tokens == 1000
    ledger.check_invariant()


def test_a_free_model_is_expressible_and_distinct_from_unknown() -> None:
    """Zero is a legitimate price. The point is that it must be DECLARED."""
    assert CostLedger(RATES).price("local-7b", 10_000, 10_000) == 0


def test_pricing_rounds_up() -> None:
    """An error in a spend figure must never be the reassuring direction."""
    ledger = CostLedger({"m": ModelRate(1, 0)})
    assert ledger.price("m", 1, 0) == 1, "1 token at 1 microcent/1k rounds UP to 1"


def test_input_and_output_are_priced_separately() -> None:
    ledger = CostLedger(RATES)
    assert ledger.price("big", 1000, 0) == 300
    assert ledger.price("big", 0, 1000) == 1500


def test_agent_and_model_views_reconcile() -> None:
    ledger = CostLedger(RATES)
    ledger.record("KRYOS", "big", 1000, 500)
    ledger.record("PRAX", "big", 2000, 100)
    ledger.record("PRAX", "local-7b", 9000, 9000)
    ledger.check_invariant()
    assert sum(t.calls for t in ledger.by_agent().values()) == 3
    assert ledger.by_agent()["PRAX"].calls == 2
    assert ledger.by_model()["big"].calls == 2


def test_costs_are_integers_not_floats() -> None:
    """Float money accumulates a total that disagrees with its own line items."""
    ledger = CostLedger(RATES)
    total = sum(ledger.record("A", "big", 333, 777) for _ in range(1000))
    assert isinstance(total, int)
    assert total == ledger.total_microcents


def test_negative_inputs_are_refused() -> None:
    ledger = CostLedger(RATES)
    with pytest.raises(ValueError):
        ledger.price("big", -1, 0)
    with pytest.raises(ValueError):
        ModelRate(-1, 0)
    with pytest.raises(ValueError):
        ledger.record("", "big", 1, 1)


# ==========================================================================
# #51 trajectory — append-only, redacted on the way in
# ==========================================================================
def test_secrets_are_redacted_before_reaching_the_stream() -> None:
    """Redacting at read time means the secret is already on disk."""
    buf = io.StringIO()
    w = TrajectoryWriter(buf, "run1")
    w.append("call", api_key="ghp_aaaaaaaaaaaaaaaaaaaa", model="big")
    written = buf.getvalue()
    assert "ghp_" not in written
    assert REDACTED in written
    assert "big" in written, "redaction must not eat ordinary fields"


def test_secret_shaped_value_is_caught_under_an_innocent_key() -> None:
    """The key name is a hint, not the evidence."""
    buf = io.StringIO()
    TrajectoryWriter(buf, "run1").append("note", comment="use ghp_bbbbbbbbbbbbbbbbbbbb")
    assert "ghp_" not in buf.getvalue()


def test_returned_record_is_the_redacted_one() -> None:
    """A caller echoing the return value must not re-leak what was scrubbed."""
    rec = TrajectoryWriter(io.StringIO(), "r").append("x", token="sk-aaaaaaaaaaaaaaaaaa")
    assert rec["token"] == REDACTED


def test_redaction_is_depth_bounded_on_a_cycle() -> None:
    """A logger that can kill the run it is logging is worse than none."""
    d: dict = {"a": 1}
    d["self"] = d
    assert "TRUNCATED" in json.dumps(redact(d))


def test_oversized_field_is_truncated_with_the_original_size_stated() -> None:
    out = redact("x" * (MAX_FIELD_CHARS + 500))
    assert len(out) < MAX_FIELD_CHARS + 200
    assert "truncated" in out and str(MAX_FIELD_CHARS + 500) in out


def test_every_line_is_valid_json_and_sequence_numbered() -> None:
    buf = io.StringIO()
    w = TrajectoryWriter(buf, "run7")
    for i in range(5):
        w.append("step", i=i)
    records = [json.loads(ln) for ln in buf.getvalue().splitlines()]
    assert [r["seq"] for r in records] == [1, 2, 3, 4, 5]
    assert {r["run_id"] for r in records} == {"run7"}


def test_writer_offers_no_mutation_path() -> None:
    """Append-only by construction, not by convention."""
    for forbidden in ("update", "delete", "rewrite", "truncate", "edit", "remove"):
        assert not hasattr(TrajectoryWriter, forbidden)


def test_truncated_final_line_loses_only_that_line() -> None:
    """The entire reason for JSONL over one big JSON array."""
    buf = io.StringIO()
    w = TrajectoryWriter(buf, "r")
    w.append("a"); w.append("b")
    mangled = buf.getvalue() + '{"run_id":"r","seq":3,"even'
    got = list(read_trajectory(io.StringIO(mangled)))
    assert [r["event"] for r in got] == ["a", "b"]


def test_corruption_mid_file_is_not_tolerated() -> None:
    """A bad line with good lines after it is corruption, not a truncated tail."""
    buf = io.StringIO()
    w = TrajectoryWriter(buf, "r")
    w.append("a")
    lines = buf.getvalue().splitlines()
    bad = "\n".join([lines[0], "{not json", '{"event":"c"}']) + "\n"
    with pytest.raises(ValueError, match="mid-file corruption"):
        list(read_trajectory(io.StringIO(bad)))


def test_unnamed_run_is_refused() -> None:
    with pytest.raises(ValueError, match="run_id"):
        TrajectoryWriter(io.StringIO(), "  ")


def test_unnamed_event_is_refused() -> None:
    with pytest.raises(ValueError):
        TrajectoryWriter(io.StringIO(), "r").append("")


def test_unserialisable_value_does_not_kill_the_run() -> None:
    class Weird:
        def __repr__(self) -> str:
            return "<weird>"

    rec = TrajectoryWriter(io.StringIO(), "r").append("x", thing=Weird())
    assert "weird" in json.dumps(rec)
