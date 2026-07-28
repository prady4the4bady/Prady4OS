"""D-02 discriminating gate — long-horizon goal planner."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from aether.agents.planner.long_horizon_planner import (
    BLOCKED_ESCALATION_S,
    CHECKPOINT_EVERY,
    Goal,
    LongHorizonPlanner,
    PlanError,
    PlanMonitor,
    TaskNode,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox

GOAL = Goal(goal_id="g1", description="ship the federation layer", priority=4)


def _tree() -> list[dict[str, Any]]:
    """A well-formed 2-level tree."""
    return [
        {"node_id": "root", "description": "ship it", "estimated_hours": 0.0},
        {
            "node_id": "design", "description": "design the protocol",
            "parent_id": "root", "estimated_hours": 8.0,
        },
        {
            "node_id": "build", "description": "build it",
            "parent_id": "root", "estimated_hours": 24.0,
            "dependencies": ["design"],
        },
    ]


def _planner(
    tmp_path: Path, tree=None, granted: bool = True
) -> LongHorizonPlanner:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("D-02", "long_horizon_planner.py", "CAP_PLAN_WRITE")
    return LongHorizonPlanner(
        lockbox=lb,
        decompose_fn=lambda _g, _c: (tree if tree is not None else _tree()),
        store_path=tmp_path / "plans.jsonl", audit=audit,
    )


def test_decompose_no_cycle(tmp_path: Path) -> None:
    """The mandated gate: a 2-level goal decomposes, no cycles, persisted."""
    planner = _planner(tmp_path)
    plan = planner.decompose(GOAL)
    assert len(plan.nodes) == 3
    assert plan.total_hours == 32.0
    assert plan.by_id("build").dependencies == ("design",)   # type: ignore[union-attr]
    lines = (tmp_path / "plans.jsonl").read_text(encoding="utf-8").splitlines()
    assert len(lines) == 1


def test_cycle_is_rejected_and_named(tmp_path: Path) -> None:
    """The discriminator.

    A model will happily emit a cycle. A planner that stores it produces a plan
    that can never complete, and the failure only surfaces when execution
    wedges. The error must also NAME the cycle, not just say "invalid".
    """
    cyclic = [
        {"node_id": "a", "description": "a", "dependencies": ["c"]},
        {"node_id": "b", "description": "b", "dependencies": ["a"]},
        {"node_id": "c", "description": "c", "dependencies": ["b"]},
    ]
    planner = _planner(tmp_path, tree=cyclic)
    with pytest.raises(PlanError, match="dependency cycle") as excinfo:
        planner.decompose(GOAL)
    message = str(excinfo.value)
    assert "->" in message
    for node in ("a", "b", "c"):
        assert node in message


def test_self_dependency_rejected(tmp_path: Path) -> None:
    """The degenerate cycle a naive pairwise edge check misses."""
    planner = _planner(
        tmp_path,
        tree=[{"node_id": "a", "description": "a", "dependencies": ["a"]}],
    )
    with pytest.raises(PlanError, match="depends on itself"):
        planner.decompose(GOAL)


def test_dangling_dependency_rejected(tmp_path: Path) -> None:
    """A dependency on a task that does not exist is unsatisfiable."""
    planner = _planner(
        tmp_path,
        tree=[{"node_id": "a", "description": "a", "dependencies": ["ghost"]}],
    )
    with pytest.raises(PlanError, match="unknown task"):
        planner.decompose(GOAL)


def test_unknown_parent_rejected(tmp_path: Path) -> None:
    planner = _planner(
        tmp_path,
        tree=[{"node_id": "a", "description": "a", "parent_id": "nowhere"}],
    )
    with pytest.raises(PlanError, match="unknown parent"):
        planner.decompose(GOAL)


def test_depth_cap_enforced(tmp_path: Path) -> None:
    deep = [{"node_id": "n0", "description": "root"}]
    for i in range(1, 12):
        deep.append(
            {"node_id": f"n{i}", "description": f"level {i}", "parent_id": f"n{i - 1}"}
        )
    planner = _planner(tmp_path, tree=deep)
    with pytest.raises(PlanError, match="exceeds|deeper than"):
        planner.decompose(GOAL)


def test_checkpoints_every_third_level(tmp_path: Path) -> None:
    chain = [{"node_id": "n0", "description": "root"}]
    for i in range(1, 7):
        chain.append(
            {"node_id": f"n{i}", "description": f"level {i}", "parent_id": f"n{i - 1}"}
        )
    plan = _planner(tmp_path, tree=chain).decompose(GOAL)
    checkpoints = {n.node_id for n in plan.checkpoints}
    assert checkpoints == {"n3", "n6"}
    for node in plan.nodes:
        assert node.checkpoint == (node.depth > 0 and node.depth % CHECKPOINT_EVERY == 0)


def test_duplicate_node_id_rejected(tmp_path: Path) -> None:
    planner = _planner(
        tmp_path,
        tree=[
            {"node_id": "a", "description": "first"},
            {"node_id": "a", "description": "second"},
        ],
    )
    with pytest.raises(PlanError, match="duplicate node_id"):
        planner.decompose(GOAL)


def test_empty_decomposition_rejected(tmp_path: Path) -> None:
    planner = _planner(tmp_path, tree=[])
    with pytest.raises(PlanError, match="non-empty list"):
        planner.decompose(GOAL)


def test_decomposition_failure_wrapped(tmp_path: Path) -> None:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    lb.register("D-02", "p.py", "CAP_PLAN_WRITE")

    def explode(_g: Goal, _c: dict[str, Any]) -> list[dict[str, Any]]:
        raise RuntimeError("model offline")

    planner = LongHorizonPlanner(
        lockbox=lb, decompose_fn=explode,
        store_path=tmp_path / "plans.jsonl", audit=audit,
    )
    with pytest.raises(PlanError, match="decomposition failed"):
        planner.decompose(GOAL)


def test_goal_priority_validated() -> None:
    with pytest.raises(PlanError, match="priority"):
        Goal(goal_id="g", description="d", priority=9)


def test_capability_required(tmp_path: Path) -> None:
    planner = _planner(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_PLAN_WRITE"):
        planner.decompose(GOAL)


def test_blocked_task_escalates_with_injected_clock(tmp_path: Path) -> None:
    """A 24h rule that needs a 24h wait to exercise is never exercised."""
    clock = {"t": 1_000_000.0}
    plan = _planner(tmp_path).decompose(GOAL)
    escalated: list[TaskNode] = []
    monitor = PlanMonitor(
        plan=plan, audit=AuditLog(tmp_path / "audit_log.jsonl"),
        now_fn=lambda: clock["t"], escalate_fn=escalated.append,
    )
    monitor.mark("build", "blocked")
    assert monitor.check_progress() == []            # not yet past the window

    clock["t"] += BLOCKED_ESCALATION_S + 3600.0
    out = monitor.check_progress()
    assert [n.node_id for n in out] == ["build"]
    assert escalated and escalated[0].node_id == "build"


def test_unblocking_clears_the_timer(tmp_path: Path) -> None:
    clock = {"t": 1_000_000.0}
    plan = _planner(tmp_path).decompose(GOAL)
    monitor = PlanMonitor(plan=plan, now_fn=lambda: clock["t"])
    monitor.mark("build", "blocked")
    clock["t"] += BLOCKED_ESCALATION_S + 3600.0
    monitor.mark("build", "in_progress")
    assert monitor.check_progress() == []


def test_progress_fraction(tmp_path: Path) -> None:
    plan = _planner(tmp_path).decompose(GOAL)
    monitor = PlanMonitor(plan=plan)
    assert monitor.progress == 0.0
    monitor.mark("design", "complete")
    assert monitor.progress == pytest.approx(1 / 3, abs=1e-3)


def test_unknown_task_mark_raises(tmp_path: Path) -> None:
    plan = _planner(tmp_path).decompose(GOAL)
    with pytest.raises(PlanError, match="unknown task"):
        PlanMonitor(plan=plan).mark("ghost", "blocked")


def test_plan_is_audited(tmp_path: Path) -> None:
    _planner(tmp_path).decompose(GOAL)
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "D-02" and e["action"] == "plan.decompose" for e in entries)
