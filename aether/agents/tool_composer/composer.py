"""C-03 — emergent tool composition.

Chains registered tools into novel pipelines at runtime, without being told the
chain. Nodes are tools; an edge exists when the upstream tool's output schema
**satisfies** the downstream tool's input schema.

Two things carry the weight here:

* **Schema compatibility is structural, not nominal.** ``output ⊇ input`` means
  every required input field is present with a matching type. Comparing schema
  *names* would chain tools that happen to share a label and refuse tools that
  are genuinely compatible.
* **The DAG must stay acyclic and bounded.** Composition is greedy and capped at
  ``MAX_CHAIN`` (6) tools, and a tool is used at most once per chain. Without
  the visited set, two tools whose schemas satisfy each other compose into an
  infinite ping-pong that only shows up at execution time.

Embedding/scoring is injected. The default scorer is a deterministic token
overlap, so the gate does not need a live model and composition stays
reproducible.
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterable

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "ToolManifest",
    "ToolRegistry",
    "DAGNode",
    "ToolDAG",
    "ComposerEngine",
    "CompositionError",
    "MAX_CHAIN",
    "schema_satisfies",
]

DDR_ID = "C-03"
_FILE = "aether/agents/tool_composer/composer.py"
CAPABILITY = "CAP_TOOL_COMPOSE"

MAX_CHAIN = 6
DEFAULT_PIPELINES = Path(__file__).resolve().parent / "pipelines.jsonl"


class CompositionError(RuntimeError):
    """Composition or execution failed."""


@dataclass(slots=True, frozen=True)
class ToolManifest:
    """What a tool consumes and produces."""

    tool_id: str
    input_schema: dict[str, str] = field(default_factory=dict)
    output_schema: dict[str, str] = field(default_factory=dict)
    capability_tags: tuple[str, ...] = ()
    description: str = ""


def schema_satisfies(produced: dict[str, str], required: dict[str, str]) -> bool:
    """True when ``produced`` supplies every field ``required`` needs.

    Structural: field names AND types must match. A tool producing extra fields
    still satisfies a narrower input, which is what makes chains composable.
    """
    for name, type_name in required.items():
        if produced.get(name) != type_name:
            return False
    return True


@dataclass(slots=True)
class DAGNode:
    tool_id: str
    depth: int


@dataclass(slots=True)
class ToolDAG:
    goal: str
    nodes: list[DAGNode] = field(default_factory=list)
    edges: list[tuple[str, str]] = field(default_factory=list)

    def __len__(self) -> int:
        return len(self.nodes)

    @property
    def tool_ids(self) -> tuple[str, ...]:
        return tuple(n.tool_id for n in self.nodes)

    def to_json(self) -> str:
        return json.dumps(
            {
                "goal": self.goal,
                "nodes": [asdict(n) for n in self.nodes],
                "edges": [list(e) for e in self.edges],
                "ts": time.time(),
            },
            sort_keys=True,
            separators=(",", ":"),
        )


class ToolRegistry:
    """Tools available for composition."""

    def __init__(self) -> None:
        self._tools: dict[str, ToolManifest] = {}

    def register(self, manifest: ToolManifest) -> ToolManifest:
        if not manifest.tool_id:
            raise CompositionError("tool_id must be non-empty")
        self._tools[manifest.tool_id] = manifest
        return manifest

    def discover(self) -> list[ToolManifest]:
        return sorted(self._tools.values(), key=lambda m: m.tool_id)

    def get(self, tool_id: str) -> ToolManifest | None:
        return self._tools.get(tool_id)

    def __len__(self) -> int:
        return len(self._tools)


#: Scores a tool against a goal in [0, 1]. Injected so gates need no model.
ScoreFn = Callable[[str, ToolManifest], float]


def token_overlap_score(goal: str, manifest: ToolManifest) -> float:
    """Deterministic default: fraction of goal tokens the tool mentions."""
    goal_tokens = {t for t in goal.lower().split() if len(t) > 2}
    if not goal_tokens:
        return 0.0
    text = f"{manifest.tool_id} {manifest.description} {' '.join(manifest.capability_tags)}"
    tool_tokens = {t for t in text.lower().replace("_", " ").split() if len(t) > 2}
    return len(goal_tokens & tool_tokens) / len(goal_tokens)


class ComposerEngine:
    """Builds and runs composed pipelines."""

    def __init__(
        self,
        registry: ToolRegistry,
        lockbox: Lockbox,
        score_fn: ScoreFn | None = None,
        pipelines_path: str | Path | None = None,
        audit: AuditLog | None = None,
        max_chain: int = MAX_CHAIN,
    ) -> None:
        self.registry = registry
        self.lockbox = lockbox
        self.score_fn: ScoreFn = score_fn if score_fn is not None else token_overlap_score
        self.pipelines_path = (
            Path(pipelines_path) if pipelines_path is not None else DEFAULT_PIPELINES
        )
        self.pipelines_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self.max_chain = int(max_chain)

    def compose(self, goal: str, min_score: float = 0.01) -> ToolDAG:
        """Greedily chain tools that score for ``goal`` and fit together."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not goal:
            raise CompositionError("goal must be non-empty")

        scored = sorted(
            (
                (self.score_fn(goal, m), m)
                for m in self.registry.discover()
            ),
            key=lambda pair: (-pair[0], pair[1].tool_id),
        )
        candidates = [m for score, m in scored if score >= min_score]
        if not candidates:
            raise CompositionError(f"no tool scores for goal: {goal!r}")

        dag = ToolDAG(goal=goal)
        head = candidates[0]
        dag.nodes.append(DAGNode(tool_id=head.tool_id, depth=0))
        # A tool may appear once per chain. Two tools whose schemas satisfy each
        # other would otherwise ping-pong until the depth cap, producing a chain
        # that is bounded but nonsense.
        visited: set[str] = {head.tool_id}
        produced = dict(head.output_schema)

        while len(dag.nodes) < self.max_chain:
            nxt = None
            for manifest in candidates:
                if manifest.tool_id in visited:
                    continue
                if schema_satisfies(produced, manifest.input_schema):
                    nxt = manifest
                    break
            if nxt is None:
                break
            dag.edges.append((dag.nodes[-1].tool_id, nxt.tool_id))
            dag.nodes.append(DAGNode(tool_id=nxt.tool_id, depth=len(dag.nodes)))
            visited.add(nxt.tool_id)
            produced.update(nxt.output_schema)

        with self.pipelines_path.open("a", encoding="utf-8") as fh:
            fh.write(dag.to_json() + "\n")
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="composer.compose", gate_status="composed",
            goal=goal, tools=list(dag.tool_ids), edges=len(dag.edges),
        )
        return dag

    def execute(
        self,
        dag: ToolDAG,
        runners: dict[str, Callable[[dict[str, Any]], dict[str, Any]]],
        initial: dict[str, Any] | None = None,
    ) -> list[dict[str, Any]]:
        """Run each node in order, threading outputs into the next input."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        state: dict[str, Any] = dict(initial or {})
        outputs: list[dict[str, Any]] = []
        for node in dag.nodes:
            runner = runners.get(node.tool_id)
            if runner is None:
                raise CompositionError(f"no runner for tool: {node.tool_id}")
            try:
                result = runner(state)
            except Exception as exc:
                self.audit.append_action(
                    ddr=DDR_ID, file=_FILE, action="composer.execute",
                    gate_status="error", tool=node.tool_id,
                    error=f"{type(exc).__name__}: {exc}",
                )
                raise CompositionError(
                    f"tool {node.tool_id} failed: {type(exc).__name__}: {exc}"
                ) from exc
            if not isinstance(result, dict):
                raise CompositionError(
                    f"tool {node.tool_id} returned {type(result).__name__}, expected dict"
                )
            outputs.append(result)
            state.update(result)
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="composer.execute", gate_status="ok",
            tools=list(dag.tool_ids),
        )
        return outputs

    def pipelines(self) -> list[dict[str, Any]]:
        if not self.pipelines_path.is_file():
            return []
        out: list[dict[str, Any]] = []
        with self.pipelines_path.open("r", encoding="utf-8") as fh:
            for line in fh:
                if line.strip():
                    out.append(json.loads(line))
        return out


def default_tools() -> Iterable[ToolManifest]:
    """A tiny starter set so composition has something to chain."""
    return (
        ToolManifest(
            tool_id="fetch_document",
            input_schema={"url": "str"},
            output_schema={"raw_text": "str"},
            capability_tags=("network", "read"),
            description="fetch a document from a url",
        ),
        ToolManifest(
            tool_id="summarise_text",
            input_schema={"raw_text": "str"},
            output_schema={"summary": "str"},
            capability_tags=("model",),
            description="summarise raw text",
        ),
    )
