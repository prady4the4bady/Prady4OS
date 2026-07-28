"""D-02 — long-horizon goal planner.

Decomposes multi-week goals into a dependency tree with estimates and
checkpoints.

The validation is the feature. A model asked for a task tree will happily emit
one containing a cycle, a dependency on a node that does not exist, or a
fourteen-level chain — and a planner that stores it unchecked produces a plan
that can never complete, which surfaces only when execution wedges. So:

* **cycles are rejected** by an explicit DFS with a colour marking, and the
  error names the cycle path rather than saying "invalid";
* **dangling dependencies are rejected** — a node depending on an id that is not
  in the tree is unsatisfiable, not merely odd;
* **depth is capped at 7**, and self-dependency is caught as its own case
  because it is the degenerate cycle a naive edge check misses.

``PlanMonitor.check_progress`` escalates any task blocked longer than the window
onto the agent bus at PRIORITY_HIGH, with an injected clock so the rule is
testable.
"""

from __future__ import annotations

import json
import time
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Callable, Literal

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "Goal",
    "TaskNode",
    "Plan",
    "LongHorizonPlanner",
    "PlanMonitor",
    "PlanError",
    "MAX_DEPTH",
    "CHECKPOINT_EVERY",
    "BLOCKED_ESCALATION_S",
]

DDR_ID = "D-02"
_FILE = "aether/agents/planner/long_horizon_planner.py"
CAPABILITY = "CAP_PLAN_WRITE"

DEFAULT_STORE = Path(__file__).resolve().parent / "plans.jsonl"
MAX_DEPTH = 7
CHECKPOINT_EVERY = 3
BLOCKED_ESCALATION_S = 24 * 3600.0

TaskStatus = Literal["pending", "in_progress", "complete", "blocked"]


class PlanError(ValueError):
    """Plan invalid or refused."""


@dataclass(slots=True)
class Goal:
    goal_id: str
    description: str
    deadline_ts: float = 0.0
    priority: int = 3
    owner_agent_id: str = ""

    def __post_init__(self) -> None:
        if not 1 <= self.priority <= 5:
            raise PlanError(f"priority {self.priority} out of range 1..5")


@dataclass(slots=True)
class TaskNode:
    node_id: str
    description: str
    parent_id: str | None = None
    estimated_hours: float = 0.0
    dependencies: tuple[str, ...] = ()
    checkpoint: bool = False
    status: TaskStatus = "pending"
    depth: int = 0
    blocked_since_ts: float = 0.0


@dataclass(slots=True)
class Plan:
    goal: Goal
    nodes: list[TaskNode] = field(default_factory=list)
    ts: float = field(default_factory=time.time)

    @property
    def total_hours(self) -> float:
        return round(sum(n.estimated_hours for n in self.nodes), 2)

    @property
    def checkpoints(self) -> tuple[TaskNode, ...]:
        return tuple(n for n in self.nodes if n.checkpoint)

    def by_id(self, node_id: str) -> TaskNode | None:
        for node in self.nodes:
            if node.node_id == node_id:
                return node
        return None

    def to_json(self) -> str:
        return json.dumps(
            {
                "goal": asdict(self.goal),
                "nodes": [asdict(n) for n in self.nodes],
                "ts": self.ts,
            },
            sort_keys=True,
            separators=(",", ":"),
        )


#: Produces a raw task tree for a goal. Injected so gates are hermetic.
DecomposeFn = Callable[[Goal, dict[str, Any]], list[dict[str, Any]]]


def _detect_cycle(nodes: list[TaskNode]) -> list[str] | None:
    """Return a cycle path if the dependency graph has one.

    Three-colour DFS: WHITE unvisited, GREY on the current stack, BLACK done.
    Meeting a GREY node is a back edge, i.e. a cycle.
    """
    graph = {n.node_id: list(n.dependencies) for n in nodes}
    colour: dict[str, int] = dict.fromkeys(graph, 0)
    stack: list[str] = []

    def visit(node_id: str) -> list[str] | None:
        colour[node_id] = 1
        stack.append(node_id)
        for dep in graph.get(node_id, ()):
            if dep not in colour:
                continue                      # dangling; reported separately
            if colour[dep] == 1:
                start = stack.index(dep)
                return [*stack[start:], dep]
            if colour[dep] == 0:
                found = visit(dep)
                if found is not None:
                    return found
        colour[node_id] = 2
        stack.pop()
        return None

    for node_id in graph:
        if colour[node_id] == 0:
            found = visit(node_id)
            if found is not None:
                return found
    return None


class LongHorizonPlanner:
    """Decomposes goals into validated task trees."""

    def __init__(
        self,
        lockbox: Lockbox,
        decompose_fn: DecomposeFn,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
    ) -> None:
        self.lockbox = lockbox
        self.decompose_fn = decompose_fn
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()

    def decompose(
        self, goal: Goal, capability_map: dict[str, Any] | None = None
    ) -> Plan:
        """Build and validate a plan for ``goal``."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        try:
            raw = self.decompose_fn(goal, dict(capability_map or {}))
        except Exception as exc:
            raise PlanError(
                f"decomposition failed: {type(exc).__name__}: {exc}"
            ) from exc
        if not isinstance(raw, list) or not raw:
            raise PlanError("decomposition must return a non-empty list of tasks")

        nodes: list[TaskNode] = []
        seen: set[str] = set()
        for entry in raw:
            if not isinstance(entry, dict):
                raise PlanError(f"task must be an object, got {type(entry).__name__}")
            node_id = str(entry.get("node_id") or uuid.uuid4())
            if node_id in seen:
                raise PlanError(f"duplicate node_id: {node_id}")
            seen.add(node_id)
            nodes.append(
                TaskNode(
                    node_id=node_id,
                    description=str(entry.get("description", "")),
                    parent_id=entry.get("parent_id"),
                    estimated_hours=float(entry.get("estimated_hours", 0.0)),
                    dependencies=tuple(entry.get("dependencies", ())),
                )
            )

        self._validate(nodes)
        self._assign_depth_and_checkpoints(nodes)

        plan = Plan(goal=goal, nodes=nodes)
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(plan.to_json() + "\n")
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="plan.decompose", gate_status="ok",
            goal_id=goal.goal_id, nodes=len(nodes),
            checkpoints=len(plan.checkpoints), hours=plan.total_hours,
        )
        return plan

    def _validate(self, nodes: list[TaskNode]) -> None:
        ids = {n.node_id for n in nodes}

        # Self-dependency is the degenerate cycle a naive pairwise check misses.
        for node in nodes:
            if node.node_id in node.dependencies:
                raise PlanError(f"task {node.node_id} depends on itself")

        for node in nodes:
            dangling = sorted(set(node.dependencies) - ids)
            if dangling:
                raise PlanError(
                    f"task {node.node_id} depends on unknown task(s): {dangling}"
                )
            if node.parent_id is not None and node.parent_id not in ids:
                raise PlanError(
                    f"task {node.node_id} has unknown parent {node.parent_id!r}"
                )

        cycle = _detect_cycle(nodes)
        if cycle is not None:
            raise PlanError(f"dependency cycle: {' -> '.join(cycle)}")

    def _assign_depth_and_checkpoints(self, nodes: list[TaskNode]) -> None:
        by_id = {n.node_id: n for n in nodes}

        def depth_of(node: TaskNode, guard: int = 0) -> int:
            if guard > MAX_DEPTH + 1:
                raise PlanError(f"task tree deeper than {MAX_DEPTH} levels")
            if node.parent_id is None:
                return 0
            parent = by_id.get(node.parent_id)
            if parent is None:
                return 0
            return depth_of(parent, guard + 1) + 1

        for node in nodes:
            node.depth = depth_of(node)
            if node.depth > MAX_DEPTH:
                raise PlanError(
                    f"task {node.node_id} at depth {node.depth} exceeds {MAX_DEPTH}"
                )
            # A checkpoint every third level, so progress is observable without
            # waiting for the whole tree.
            node.checkpoint = node.depth > 0 and node.depth % CHECKPOINT_EVERY == 0


class PlanMonitor:
    """Watches a plan and escalates long-blocked tasks."""

    def __init__(
        self,
        plan: Plan,
        audit: AuditLog | None = None,
        now_fn: Callable[[], float] | None = None,
        escalate_fn: Callable[[TaskNode], None] | None = None,
        blocked_after_s: float = BLOCKED_ESCALATION_S,
    ) -> None:
        self.plan = plan
        self.audit = audit if audit is not None else AuditLog()
        self.now_fn: Callable[[], float] = now_fn if now_fn is not None else time.time
        self.escalate_fn = escalate_fn
        self.blocked_after_s = float(blocked_after_s)

    def mark(self, node_id: str, status: TaskStatus) -> TaskNode:
        node = self.plan.by_id(node_id)
        if node is None:
            raise PlanError(f"unknown task: {node_id}")
        node.status = status
        node.blocked_since_ts = self.now_fn() if status == "blocked" else 0.0
        return node

    def check_progress(self) -> list[TaskNode]:
        """Escalate anything blocked past the window."""
        now = self.now_fn()
        escalated: list[TaskNode] = []
        for node in self.plan.nodes:
            if node.status != "blocked" or not node.blocked_since_ts:
                continue
            if (now - node.blocked_since_ts) > self.blocked_after_s:
                escalated.append(node)
                self.audit.append_action(
                    ddr=DDR_ID, file=_FILE, action="plan.blocked_escalation",
                    gate_status="escalated", node_id=node.node_id,
                    blocked_hours=round((now - node.blocked_since_ts) / 3600.0, 1),
                )
                if self.escalate_fn is not None:
                    self.escalate_fn(node)
        return escalated

    @property
    def progress(self) -> float:
        if not self.plan.nodes:
            return 0.0
        done = sum(1 for n in self.plan.nodes if n.status == "complete")
        return round(done / len(self.plan.nodes), 4)
