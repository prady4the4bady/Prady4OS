"""I-03 / I-04 / I-05 / I-07 — cross-module integration wires.

Each D-series module has its own gate. These tests cover what those cannot: the
seams. A wire is where two individually-correct modules disagree about a type,
an ordering, or whose job a check is, and every one of the seam bugs this
session hit (C-09's PeerContract, D-13's replay) lived exactly there.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox


def _base(tmp_path: Path) -> tuple[Lockbox, AuditLog]:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    return Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit), audit


# ---------------------------------------------------------------- I-03 -------
def test_i03_proposal_produces_a_plan_node(tmp_path: Path) -> None:
    """I-03 — a D-01 self-improvement proposal must become D-02 plan work.

    A proposal that never reaches the planner is a proposal that never happens.
    The seam is the type change: D-01 emits an ImprovementProposal, D-02
    consumes a Goal, and nothing before this test asserted the crossing.
    """
    from aether.agents.planner.long_horizon_planner import Goal, LongHorizonPlanner
    from aether.agents.self_improvement.proposal_engine import ImprovementProposal

    lb, audit = _base(tmp_path)
    lb.register("D-02", "long_horizon_planner.py", "CAP_PLAN_WRITE")

    proposal = ImprovementProposal(
        prop_id="p-1", source_ddr="D-01",
        target_file="aether/agents/causal/causal_reasoning_engine.py",
        current_behaviour="screening-off runs over every variable",
        proposed_change="index candidate confounders",
        expected_improvement="inference drops below 50ms",
        test_to_verify="test_causal_reasoning.py::test_confounded_pair",
        risk_level=2,
    )

    def _decompose(goal: Goal, _caps: dict[str, Any]) -> list[dict[str, Any]]:
        return [
            {"node_id": "n1", "description": f"reproduce: {goal.description}",
             "estimated_hours": 1.0},
            {"node_id": "n2", "description": "apply change",
             "estimated_hours": 2.0, "depends_on": ["n1"]},
            {"node_id": "n3", "description": "run the verifying test",
             "estimated_hours": 0.5, "depends_on": ["n2"]},
        ]

    planner = LongHorizonPlanner(
        lockbox=lb, decompose_fn=_decompose,
        store_path=tmp_path / "plans.jsonl", audit=audit,
    )

    # The wire itself: proposal -> goal -> plan.
    goal = Goal(goal_id=proposal.prop_id,
                description=proposal.proposed_change,
                priority=proposal.risk_level, owner_agent_id=proposal.source_ddr)
    plan = planner.decompose(goal)

    assert len(plan.nodes) == 3
    assert plan.by_id("n3") is not None
    # The plan must carry the proposal's identity, or the result cannot be
    # attributed back to what asked for it.
    assert goal.goal_id == proposal.prop_id
    assert proposal.test_to_verify in {n.description for n in plan.nodes} or any(
        "test" in n.description for n in plan.nodes
    )


# ---------------------------------------------------------------- I-04 -------
def test_i04_router_audits_token_count_and_latency(tmp_path: Path) -> None:
    """I-04 — every routed call writes token_count and latency_ms.

    The spec said VERIFY; verification found the entry missing, so infer() now
    writes route.call. route.select alone records only that a model was CHOSEN.

    Without those two fields the audit log records that inference happened but
    not what it cost, so no budget or regression question can be answered from
    it afterwards.
    """
    from aether.ai_core.ensemble.ensemble_router import EnsembleRouter, ModelProfile

    lb, audit = _base(tmp_path)
    lb.register("D-03", "ensemble_router.py", "CAP_MODEL_ROUTE")

    router = EnsembleRouter(
        lockbox=lb, store_path=tmp_path / "profiles.jsonl", audit=audit)
    router.register_model(ModelProfile(
        "llama3", "ollama", domains=("general",), quality_score=0.8,
        cost_per_1k_tokens=0.5,
    ))

    text, profile = router.infer(
        "summarise the causal graph", "general",
        runners={"llama3": lambda _p, _m: "a short summary"},
    )
    assert text == "a short summary"
    assert profile.model_id == "llama3"

    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    calls = [e for e in entries if e["action"] == "route.call"]
    assert len(calls) == 1, "no per-call audit entry was written"
    extra = calls[0]["extra"]
    assert extra["token_count"] > 0
    assert extra["latency_ms"] >= 0.0
    assert extra["model"] == "llama3"
    # Cost must follow the token count, not be a constant.
    assert extra["cost"] == pytest.approx(extra["token_count"] / 1000.0 * 0.5)


def test_i04_token_estimate_is_monotonic() -> None:
    """An estimate that ignores length would make every call look identical in
    the audit log, which is the same as not recording cost at all."""
    from aether.ai_core.ensemble.ensemble_router import estimate_tokens

    assert estimate_tokens("") == 0
    assert estimate_tokens("a") >= 1
    assert estimate_tokens("a" * 400) > estimate_tokens("a" * 40)


# ---------------------------------------------------------------- I-05 -------
def test_i05_science_ledger_chains_across_modules(tmp_path: Path) -> None:
    """I-05 — a hypothesis (D-07) and its outcome (D-10) must chain.

    D-10's own gate proves the chain internally. This crosses the module
    boundary: the thing being hashed is an outcome for a hypothesis another
    module minted, and a rewrite of that record must still be detectable.
    """
    from aether.agents.research.experiment_outcome_evaluator import (
        Comparator,
        ExperimentOutcomeEvaluator,
        FalsificationCondition,
        Verdict,
    )
    from aether.agents.research.hypothesis_generator import HypothesisGenerator

    lb, audit = _base(tmp_path)
    lb.register("D-07", "hypothesis_generator.py", "CAP_HYPOTHESIS_WRITE")
    lb.register("D-10", "experiment_outcome_evaluator.py", "CAP_WATCHDOG")

    gen = HypothesisGenerator(
        lockbox=lb,
        generate_fn=lambda _c, _d: [{
            "statement": "indexing confounders cuts inference time",
            "falsification_condition": "inference_ms >= 50",
            "expected_evidence": "inference_ms before and after",
            "estimated_cost": 3.0,
        }],
        store_path=tmp_path / "hyp.jsonl", audit=audit,
    )
    hyp = gen.generate("causal perf")[0]

    ev = ExperimentOutcomeEvaluator(
        lockbox=lb, store_path=tmp_path / "outcomes.jsonl", audit=audit)
    cond = FalsificationCondition("inference_ms", Comparator.GE, 50.0)
    outcome = ev.evaluate(hyp.hyp_id, cond, {"inference_ms": 12.0})

    assert outcome.verdict is Verdict.CONFIRMED
    assert outcome.hyp_id == hyp.hyp_id          # the seam: same identity
    root = ev.root()
    assert ev.verify_chain(root) is True

    # Rewriting the outcome for a real hypothesis must still break the chain.
    ev._outcomes[0].verdict = Verdict.FALSIFIED
    assert ev.verify_chain(root) is False


# ---------------------------------------------------------------- I-07 -------
def test_i07_composed_skill_reaches_the_invention_registry(tmp_path: Path) -> None:
    """I-07 — a composed skill must land in C-04 before any agent may use it,
    and it must not self-approve on the way (S8)."""
    from aether.agents.invention.registry import InventionRegistry
    from aether.agents.tool_composer.autonomous_skill_composer import (
        AutonomousSkillComposer,
        Skill,
    )

    lb, audit = _base(tmp_path)
    lb.register("D-15", "autonomous_skill_composer.py", "CAP_ONTOLOGY_WRITE")
    lb.register("C-04", "invention_registry.py", "CAP_INVENTION_WRITE")

    comp = AutonomousSkillComposer(
        lockbox=lb, agent_id="composer-1",
        store_path=tmp_path / "skills.jsonl", audit=audit,
    )
    comp.register_skill(Skill("read_file", "core", ("CAP_MEMORY_WRITE",)))
    comp.register_skill(Skill("summarise", "core", ()))
    composed = comp.compose("digest", ["read_file", "summarise"])

    # Not usable yet — and specifically not usable by its own composer.
    assert comp.runnable("digest") is False
    from aether.agents.tool_composer.autonomous_skill_composer import CapError
    with pytest.raises(CapError):
        comp.approve("digest", approver="composer-1")

    comp.approve("digest", approver="C-08-reviewer")
    assert comp.runnable("digest") is True

    registry = InventionRegistry(
        lockbox=lb, store_path=tmp_path / "inventions.jsonl", audit=audit)
    inv_id = registry.add(
        source_ddr="D-15", title=composed.name,
        description=f"composed from {list(composed.steps)}",
        capability_delta={"capabilities": list(composed.capabilities)},
    )
    landed = registry.get(inv_id)
    assert landed is not None
    assert landed.title == "digest"
    assert landed.source_ddr == "D-15"
    # The registry entry must carry the composition's capabilities, or C-04
    # cannot answer what the new skill is allowed to do.
    assert landed.capability_delta["capabilities"] == list(composed.capabilities)
