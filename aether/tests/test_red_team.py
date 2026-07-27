"""B-06 discriminating gate — automated red-team agent."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.agents.firewall.prompt_injection import PromptFirewall, Verdict
from aether.agents.red_team.red_team_agent import (
    AttackFamily,
    RedTeamAgent,
)
from aether.kernel.audit.audit_log import AuditLog


def _agent(tmp_path: Path) -> RedTeamAgent:
    return RedTeamAgent(audit=AuditLog(tmp_path / "audit_log.jsonl"))


def test_generates_seeds_and_mutations(tmp_path: Path) -> None:
    rt = _agent(tmp_path)
    cases = rt.generate(families=[AttackFamily.INSTRUCTION_OVERRIDE], max_mutations=1)
    raw = [c for c in cases if not c.mutations]
    mutated = [c for c in cases if c.mutations]
    assert len(raw) == 2                    # two seeds in that family
    assert mutated                          # and mutated variants of each
    assert all(c.family is AttackFamily.INSTRUCTION_OVERRIDE for c in cases)


def test_mutations_compose(tmp_path: Path) -> None:
    """The point of the agent: combinations, not a replay list."""
    rt = _agent(tmp_path)
    cases = rt.generate(families=[AttackFamily.EXFILTRATION], max_mutations=2)
    assert any(len(c.mutations) == 2 for c in cases)
    # Every generated case is distinct from its seed.
    assert len({c.payload for c in cases}) > 2


def test_detects_an_undefended_target(tmp_path: Path) -> None:
    """A target that defends nothing must be reported as fully escaped."""
    rt = _agent(tmp_path)
    report = rt.run(target=lambda _payload: False, max_mutations=1)
    assert report.total > 0
    assert report.defended == 0
    assert not report.clean
    assert report.defence_rate == 0.0
    assert len(report.escapes) == report.total


def test_raising_target_counts_as_defence(tmp_path: Path) -> None:
    """B-04/B-05 refuse by raising, so a raise must score as defended."""
    rt = _agent(tmp_path)

    def strict(_payload: str) -> bool:
        raise RuntimeError("blocked")

    report = rt.run(target=strict, max_mutations=0)
    assert report.clean
    assert report.defence_rate == 1.0


def test_finds_the_gap_a_substring_defender_leaves(tmp_path: Path) -> None:
    """The discriminator.

    A naive defender that does a plain lowercase substring match looks fine
    against the raw seeds, but the mutators defeat it. If the agent could only
    replay seeds, this target would score clean — so this test fails for any
    implementation that does not actually compose mutations.
    """
    rt = _agent(tmp_path)

    def substring_defender(payload: str) -> bool:
        return "ignore all previous instructions" in payload.lower()

    seeds_only = rt.generate(
        families=[AttackFamily.INSTRUCTION_OVERRIDE], max_mutations=0
    )
    seed_report = rt.run(target=substring_defender, cases=seeds_only)
    # The first seed is matched verbatim by the naive defender.
    assert seed_report.defended >= 1

    full_report = rt.run(
        target=substring_defender,
        cases=rt.generate(
            families=[AttackFamily.INSTRUCTION_OVERRIDE], max_mutations=2
        ),
    )
    assert not full_report.clean, "mutators failed to defeat a substring matcher"
    assert full_report.defence_rate < seed_report.defence_rate


def test_b04_firewall_defends_the_generated_corpus(tmp_path: Path) -> None:
    """B-04 must hold against the whole generated corpus, not just seeds."""
    fw = PromptFirewall(audit=AuditLog(tmp_path / "audit_log.jsonl"))
    rt = _agent(tmp_path)

    def firewalled(payload: str) -> bool:
        return fw.scan(payload).verdict is not Verdict.ALLOW

    report = rt.run(target=firewalled, max_mutations=2)
    assert report.clean, (
        "firewall let these through: "
        + ", ".join(r.case.case_id for r in report.escapes[:5])
    )


def test_run_is_audited(tmp_path: Path) -> None:
    audit_path = tmp_path / "audit_log.jsonl"
    rt = RedTeamAgent(audit=AuditLog(audit_path))
    rt.run(target=lambda _p: True, max_mutations=0)
    entries = AuditLog(audit_path).read_all()
    assert any(e["ddr"] == "B-06" and e["action"] == "red_team.run" for e in entries)


def test_bad_depth_rejected(tmp_path: Path) -> None:
    rt = _agent(tmp_path)
    with pytest.raises(ValueError, match="max_mutations"):
        rt.generate(max_mutations=-1)


def test_normalised_preview_folds_obfuscation(tmp_path: Path) -> None:
    rt = _agent(tmp_path)
    assert "ignore" in RedTeamAgent.normalised_preview("ig​nore")
    assert "Ignore" in RedTeamAgent.normalised_preview("Ｉｇｎｏｒｅ")
