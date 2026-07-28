"""I-01 / I-08 gate — the ensemble router (D-03) is the ONLY inference path.

These are the same gate and the spec says to run both, so they share this file.

Enforcing it as a test rather than a one-off grep is the point. A rule checked
once by hand is a rule that decays: the next agent module to need a model call
takes the direct route, the suite stays green, and the router quietly stops being
the single path — which also silently voids **S14** (no circular imports; agents
must never reach into ``aether/ai_core/``) and the per-call audit trail I-04
depends on. Routing everything through D-03 is what makes inference observable
at all.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

AGENTS_DIR = Path(__file__).resolve().parents[1] / "agents"

#: The router is the single sanctioned inference path; the bridge is the single
#: sanctioned place to speak HTTP to a model.
_BRIDGE_DIRS = {"ollama_bridge", "cloud_bridge"}

_FORBIDDEN = (
    # S14: agents must not import the inference layer directly.
    (re.compile(r"^\s*(?:from|import)\s+aether\.ai_core\b", re.M),
     "imports aether.ai_core directly — inference must go through the D-03 "
     "ensemble_router"),
    # A direct HTTP client in an agent is a direct model call in disguise.
    (re.compile(r"^\s*(?:import\s+(?:requests|httpx|urllib|http)\b"
                r"|from\s+(?:requests|httpx|urllib|http)\b)", re.M),
     "imports an HTTP client — model traffic belongs in ollama_bridge/"),
)


def _agent_sources() -> list[Path]:
    return [p for p in AGENTS_DIR.rglob("*.py")
            if "__pycache__" not in p.parts
            and not (_BRIDGE_DIRS & set(p.parts))]


def test_there_are_agent_sources_to_check() -> None:
    """Guard the guard: a glob that silently matches nothing passes everything."""
    sources = _agent_sources()
    assert len(sources) > 20, f"only found {len(sources)} agent sources"


@pytest.mark.parametrize("pattern,why", _FORBIDDEN,
                         ids=["no-direct-ai_core-import", "no-http-client"])
def test_no_agent_bypasses_the_router(pattern: re.Pattern[str], why: str) -> None:
    offenders = []
    for path in _agent_sources():
        text = path.read_text(encoding="utf-8")
        for match in pattern.finditer(text):
            line = text[:match.start()].count("\n") + 1
            offenders.append(f"{path.relative_to(AGENTS_DIR.parent)}:{line}")
    assert offenders == [], f"{why}: {offenders}"


def test_forbidden_patterns_actually_match_a_violation() -> None:
    """The patterns must be able to fail, or this file is decoration."""
    ai_core_pat, http_pat = _FORBIDDEN[0][0], _FORBIDDEN[1][0]
    assert ai_core_pat.search("from aether.ai_core.ensemble import Router\n")
    assert ai_core_pat.search("import aether.ai_core\n")
    assert http_pat.search("import requests\n")
    assert http_pat.search("from httpx import AsyncClient\n")
    # ...and must not fire on legitimate code.
    assert not ai_core_pat.search("from aether.agents.causal import x\n")
    assert not http_pat.search("# we deliberately do not import requests\n")
