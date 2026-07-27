"""C-05 discriminating gate — capability boundary mapper."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.agents.capability.boundary_mapper import (
    BoundaryError,
    BoundaryMapper,
    boundary_confidence,
)
from aether.agents.red_team.red_team_agent import AttackFamily, RedTeamAgent
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _mapper(
    tmp_path: Path,
    execute_fn=lambda _d, task: "easy" in task,
    granted: bool = True,
) -> BoundaryMapper:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("C-05", "boundary_mapper.py", "CAP_CAPABILITY_PROBE")
    return BoundaryMapper(
        lockbox=lb, execute_fn=execute_fn,
        red_team=RedTeamAgent(audit=audit),
        map_path=tmp_path / "boundary_map.jsonl", audit=audit,
    )


def test_probe_updates_map(tmp_path: Path) -> None:
    """The mandated gate: one success, one failure, both recorded."""
    bm = _mapper(tmp_path)
    assert bm.probe("text", "easy summarisation") is True
    assert bm.probe("text", "hard theorem proving") is False

    domain = bm.get("text")
    assert domain is not None
    assert domain.known_tasks == ["easy summarisation"]
    assert domain.failure_tasks == ["hard theorem proving"]
    assert domain.probes == 2
    assert domain.boundary_confidence == 0.0        # even split: least understood


def test_confidence_measures_boundary_clarity_not_capability(tmp_path: Path) -> None:
    """The discriminator.

    Scoring "confidence" as a success rate ranks a domain the system fails at
    100% of the time as maximally uncertain, sending probe effort exactly where
    it is least needed. Both all-pass and all-fail are WELL MAPPED.
    """
    assert boundary_confidence(4, 0) == 1.0         # certainly capable
    assert boundary_confidence(0, 4) == 1.0         # certainly not — equally clear
    assert boundary_confidence(2, 2) == 0.0         # the edge runs through here
    assert boundary_confidence(3, 1) == 0.5
    assert boundary_confidence(0, 0) == 0.0         # nothing known


def test_least_understood_ranks_the_true_edge_first(tmp_path: Path) -> None:
    bm = _mapper(tmp_path, execute_fn=lambda d, t: "easy" in t)
    # 'vision' is all-fail: clear boundary.
    bm.probe("vision", "hard a")
    bm.probe("vision", "hard b")
    # 'text' is split: this is the real edge.
    bm.probe("text", "easy a")
    bm.probe("text", "hard b")

    ranked = bm.least_understood()
    assert ranked[0].domain == "text"
    assert ranked[0].boundary_confidence < ranked[-1].boundary_confidence


def test_a_task_that_flips_sides_moves(tmp_path: Path) -> None:
    """A task must not appear in both buckets after its result changes."""
    results = {"value": False}
    bm = _mapper(tmp_path, execute_fn=lambda _d, _t: results["value"])
    bm.probe("text", "borderline task")
    assert bm.get("text").failure_tasks == ["borderline task"]   # type: ignore[union-attr]

    results["value"] = True
    bm.probe("text", "borderline task")
    domain = bm.get("text")
    assert domain is not None
    assert domain.known_tasks == ["borderline task"]
    assert domain.failure_tasks == []
    assert domain.probes == 1


def test_a_raising_task_counts_as_a_failure(tmp_path: Path) -> None:
    """A task that explodes is a capability limit, not a mapper crash."""

    def explode(_d: str, _t: str) -> bool:
        raise RuntimeError("tool blew up")

    bm = _mapper(tmp_path, execute_fn=explode)
    assert bm.probe("text", "task") is False
    assert bm.get("text").failure_tasks == ["task"]      # type: ignore[union-attr]


def test_adversarial_probing_uses_the_red_team(tmp_path: Path) -> None:
    """Probes come from B-06 so the map is pushed outward, not re-confirmed."""
    seen: list[str] = []

    def record(_d: str, task: str) -> bool:
        seen.append(task)
        return False

    bm = _mapper(tmp_path, execute_fn=record)
    out = bm.probe_adversarial("prompt", AttackFamily.INSTRUCTION_OVERRIDE, limit=3)
    assert len(out) == 3
    assert len(seen) == 3
    assert all(isinstance(payload, str) and payload for payload, _ in out)
    assert bm.get("prompt").probes == len(set(seen))      # type: ignore[union-attr]


def test_capability_required(tmp_path: Path) -> None:
    bm = _mapper(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_CAPABILITY_PROBE"):
        bm.probe("text", "anything")


def test_probe_validates_input(tmp_path: Path) -> None:
    bm = _mapper(tmp_path)
    with pytest.raises(BoundaryError, match="needs a domain and a task"):
        bm.probe("", "task")
    with pytest.raises(BoundaryError, match="needs a domain and a task"):
        bm.probe("text", "")


def test_map_survives_restart(tmp_path: Path) -> None:
    bm = _mapper(tmp_path)
    bm.probe("text", "easy one")
    bm.probe("text", "hard one")

    reborn = _mapper(tmp_path)
    domain = reborn.get("text")
    assert domain is not None
    assert domain.known_tasks == ["easy one"]
    assert domain.failure_tasks == ["hard one"]


def test_export_map_shape(tmp_path: Path) -> None:
    bm = _mapper(tmp_path)
    bm.probe("text", "easy one")
    exported = bm.export_map()
    assert set(exported) == {"text"}
    assert exported["text"].boundary_confidence == 1.0


def test_probes_are_audited(tmp_path: Path) -> None:
    bm = _mapper(tmp_path)
    bm.probe("text", "easy one")
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "C-05" and e["action"] == "boundary.update" for e in entries)


def test_corrupt_map_reported(tmp_path: Path) -> None:
    (tmp_path / "boundary_map.jsonl").write_text("{nope\n", encoding="utf-8")
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    with pytest.raises(BoundaryError, match="corrupt boundary map"):
        BoundaryMapper(
            lockbox=lb, execute_fn=lambda _d, _t: True,
            map_path=tmp_path / "boundary_map.jsonl", audit=audit,
        )
