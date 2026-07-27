"""C-03 discriminating gate — emergent tool composition."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from aether.agents.tool_composer.composer import (
    MAX_CHAIN,
    ComposerEngine,
    CompositionError,
    ToolManifest,
    ToolRegistry,
    default_tools,
    schema_satisfies,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _engine(tmp_path: Path, registry: ToolRegistry, granted: bool = True) -> ComposerEngine:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("C-03", "composer.py", "CAP_TOOL_COMPOSE")
    return ComposerEngine(
        registry=registry, lockbox=lb,
        pipelines_path=tmp_path / "pipelines.jsonl", audit=audit,
    )


def test_compose_two_tools(tmp_path: Path) -> None:
    """The mandated gate: two compatible tools chain into a 2-node DAG."""
    reg = ToolRegistry()
    for manifest in default_tools():
        reg.register(manifest)
    engine = _engine(tmp_path, reg)

    dag = engine.compose("fetch and summarise a document")
    assert len(dag) == 2
    assert dag.edges == [("fetch_document", "summarise_text")]
    assert dag.tool_ids == ("fetch_document", "summarise_text")


def test_incompatible_tools_do_not_chain(tmp_path: Path) -> None:
    """The discriminator.

    These two tools both score for the goal but their schemas do not fit. A
    composer matching on relevance alone chains them and produces a pipeline
    that fails at execution; structural checking refuses the edge up front.
    """
    reg = ToolRegistry()
    reg.register(
        ToolManifest(
            tool_id="fetch_document",
            input_schema={"url": "str"},
            output_schema={"raw_text": "str"},
            description="fetch document",
        )
    )
    reg.register(
        ToolManifest(
            tool_id="resize_document_image",
            input_schema={"image_bytes": "bytes"},     # nothing produces this
            output_schema={"thumbnail": "bytes"},
            description="resize document image",
        )
    )
    engine = _engine(tmp_path, reg)
    dag = engine.compose("fetch document")
    assert len(dag) == 1
    assert dag.edges == []


def test_type_mismatch_blocks_an_edge(tmp_path: Path) -> None:
    """Same field name, different type: nominal matching would chain these."""
    reg = ToolRegistry()
    reg.register(
        ToolManifest("producer", {}, {"payload": "str"}, description="produce payload")
    )
    reg.register(
        ToolManifest("consumer", {"payload": "bytes"}, {"done": "bool"},
                     description="consume payload")
    )
    engine = _engine(tmp_path, reg)
    assert engine.compose("produce payload").edges == []


def test_mutually_compatible_tools_do_not_pingpong(tmp_path: Path) -> None:
    """Without a visited set these two compose until the depth cap."""
    reg = ToolRegistry()
    reg.register(ToolManifest("a2b", {"a": "str"}, {"b": "str"}, description="convert a b"))
    reg.register(ToolManifest("b2a", {"b": "str"}, {"a": "str"}, description="convert b a"))
    engine = _engine(tmp_path, reg)
    dag = engine.compose("convert a b")
    assert len(dag) <= 2
    assert len(set(dag.tool_ids)) == len(dag.tool_ids), "a tool repeated in one chain"


def test_chain_is_capped(tmp_path: Path) -> None:
    reg = ToolRegistry()
    # A long ladder: each tool consumes the previous field and adds the next.
    for i in range(12):
        reg.register(
            ToolManifest(
                tool_id=f"step{i}",
                input_schema={} if i == 0 else {f"f{i}": "str"},
                output_schema={f"f{i + 1}": "str"},
                description="ladder step convert",
            )
        )
    engine = _engine(tmp_path, reg)
    dag = engine.compose("ladder step convert")
    assert len(dag) <= MAX_CHAIN


def test_execute_threads_state(tmp_path: Path) -> None:
    reg = ToolRegistry()
    for manifest in default_tools():
        reg.register(manifest)
    engine = _engine(tmp_path, reg)
    dag = engine.compose("fetch and summarise a document")

    def fetch(state: dict[str, Any]) -> dict[str, Any]:
        return {"raw_text": f"contents of {state['url']}"}

    def summarise(state: dict[str, Any]) -> dict[str, Any]:
        return {"summary": state["raw_text"][:8]}

    outputs = engine.execute(
        dag,
        runners={"fetch_document": fetch, "summarise_text": summarise},
        initial={"url": "doc://x"},
    )
    assert outputs[0]["raw_text"] == "contents of doc://x"
    assert outputs[1]["summary"] == "contents"


def test_execute_reports_a_failing_tool(tmp_path: Path) -> None:
    reg = ToolRegistry()
    reg.register(ToolManifest("boom", {}, {"x": "str"}, description="boom tool"))
    engine = _engine(tmp_path, reg)
    dag = engine.compose("boom tool")

    def explode(_state: dict[str, Any]) -> dict[str, Any]:
        raise ValueError("tool exploded")

    with pytest.raises(CompositionError, match="boom failed"):
        engine.execute(dag, runners={"boom": explode})


def test_execute_requires_a_runner(tmp_path: Path) -> None:
    reg = ToolRegistry()
    reg.register(ToolManifest("lonely", {}, {"x": "str"}, description="lonely tool"))
    engine = _engine(tmp_path, reg)
    dag = engine.compose("lonely tool")
    with pytest.raises(CompositionError, match="no runner"):
        engine.execute(dag, runners={})


def test_capability_required(tmp_path: Path) -> None:
    reg = ToolRegistry()
    for manifest in default_tools():
        reg.register(manifest)
    engine = _engine(tmp_path, reg, granted=False)
    with pytest.raises(CapabilityError, match="CAP_TOOL_COMPOSE"):
        engine.compose("fetch and summarise a document")


def test_no_matching_tool_is_an_error(tmp_path: Path) -> None:
    reg = ToolRegistry()
    reg.register(ToolManifest("unrelated", {}, {"z": "str"}, description="quantum widget"))
    engine = _engine(tmp_path, reg)
    with pytest.raises(CompositionError, match="no tool scores"):
        engine.compose("bake a sourdough loaf")


def test_pipelines_are_persisted_and_audited(tmp_path: Path) -> None:
    reg = ToolRegistry()
    for manifest in default_tools():
        reg.register(manifest)
    engine = _engine(tmp_path, reg)
    engine.compose("fetch and summarise a document")
    assert engine.pipelines()[0]["goal"] == "fetch and summarise a document"
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "C-03" and e["action"] == "composer.compose" for e in entries)


@pytest.mark.parametrize(
    ("produced", "required", "expected"),
    [
        ({"a": "str"}, {"a": "str"}, True),
        ({"a": "str", "b": "int"}, {"a": "str"}, True),      # extras are fine
        ({"a": "str"}, {"a": "str", "b": "int"}, False),     # missing field
        ({"a": "int"}, {"a": "str"}, False),                 # wrong type
        ({}, {}, True),
    ],
)
def test_schema_satisfies(produced: dict[str, str], required: dict[str, str], expected: bool) -> None:
    assert schema_satisfies(produced, required) is expected


def test_registry_rejects_empty_id() -> None:
    with pytest.raises(CompositionError, match="tool_id"):
        ToolRegistry().register(ToolManifest(""))
