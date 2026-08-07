"""DDR-846 — the 8 legacy agents' skill.md files are validated, not merely present.

Runs inside the pytest job CI already executes with `-W error`, so a malformed or
over-budget skill file fails the build rather than sitting unnoticed. The point of
these tests is that a skill prompt is a *contract*: if it claims a capability the
kernel has not wired, an agent will act on that claim and get -EPERM at the worst
possible moment.
"""
from __future__ import annotations

import re
from pathlib import Path

import pytest

ROSTER = Path(__file__).resolve().parents[1] / "agents" / "roster"

# The 8 legacy kernel roster slots (DDR-707/730/737), mapped to Section G roles
# by DDR-846.
LEGACY_AGENTS = {
    "kryos": "file_agent",
    "prax": "shell_agent",
    "lumyn": "research_agent",
    "ahnis": "ocr_agent",
    "iris": "vision_agent",
    "ruflo": "healer_agent",
    "hermes": "orchestrator_agent",
    "solin": "verifier_agent",
}

# Spec bound, Section 3D #45.
MIN_TOKENS = 300
MAX_TOKENS = 2000

# Capabilities defined in kernel/cap.h but NOT wired to any enforcement path.
# A skill file naming one of these MUST also carry the not-yet-spawnable marker,
# or it is promising behaviour that cannot run.
UNWIRED_CAPS = ("CAP_EXEC", "CAP_OCR", "CAP_SCENE", "CAP_NET_BROWSE")

REQUIRED_FIELDS = ("**Role:**", "**Capabilities:**", "**Status:**")
REQUIRED_SECTIONS = ("## Refuses", "## Invariants")


def _skill_path(name: str) -> Path:
    return ROSTER / name / "skill.md"


def _estimate_tokens(text: str) -> int:
    """Words / 0.75.

    Stated rather than hidden: this is an APPROXIMATION. A real tokeniser would
    be exact, but pulling one in as a test dependency to police a prose budget
    trades a large dependency for a small amount of precision. The bound has
    ~33% headroom at both ends, so estimator error cannot flip a verdict.
    """
    return int(len(text.split()) / 0.75)


@pytest.mark.parametrize("name", sorted(LEGACY_AGENTS))
def test_skill_file_exists(name: str) -> None:
    assert _skill_path(name).is_file(), (
        f"{name} has a kernel roster slot and a UI card but no skill.md. "
        "A named slot with no defined behaviour is worse than an empty list."
    )


@pytest.mark.parametrize("name", sorted(LEGACY_AGENTS))
def test_skill_within_token_budget(name: str) -> None:
    tokens = _estimate_tokens(_skill_path(name).read_text(encoding="utf-8"))
    assert MIN_TOKENS <= tokens <= MAX_TOKENS, (
        f"{name}: ~{tokens} tokens, outside the Section 3D #45 budget "
        f"{MIN_TOKENS}-{MAX_TOKENS}."
    )


@pytest.mark.parametrize("name,role", sorted(LEGACY_AGENTS.items()))
def test_skill_declares_role_and_required_sections(name: str, role: str) -> None:
    text = _skill_path(name).read_text(encoding="utf-8")
    for field in REQUIRED_FIELDS:
        assert field in text, f"{name}: missing {field}"
    for section in REQUIRED_SECTIONS:
        assert section in text, f"{name}: missing {section}"
    assert role in text, f"{name}: does not name its Section G role '{role}'"


@pytest.mark.parametrize("name", sorted(LEGACY_AGENTS))
def test_refuses_section_is_not_empty(name: str) -> None:
    """A skill prompt that only describes capability is a wish.

    The failure mode of an agent prompt is not "does too little" — it is
    "talks itself into something S1-S8 forbids". Every agent must say what it
    declines.
    """
    text = _skill_path(name).read_text(encoding="utf-8")
    body = text.split("## Refuses", 1)[1].split("##", 1)[0]
    bullets = [ln for ln in body.splitlines() if ln.strip().startswith("-")]
    assert len(bullets) >= 3, (
        f"{name}: Refuses lists {len(bullets)} items; at least 3 expected."
    )


@pytest.mark.parametrize("name", sorted(LEGACY_AGENTS))
def test_unwired_capability_implies_not_spawnable(name: str) -> None:
    """THE check that matters.

    CAP_EXEC, CAP_OCR, CAP_SCENE and CAP_NET_BROWSE are declared in cap.h but
    wired to nothing. A skill file that names one without also declaring the
    agent unspawnable is a promise the kernel will refuse at runtime. This test
    stops a future edit from quietly making that promise.
    """
    text = _skill_path(name).read_text(encoding="utf-8")
    named = [cap for cap in UNWIRED_CAPS if cap in text]
    if not named:
        return
    assert "not yet spawnable" in text.lower() or "partially spawnable" in text.lower(), (
        f"{name}: names unwired capability {named} but does not declare itself "
        "not-yet-spawnable. That is a capability promise the kernel cannot keep."
    )


def test_roster_holds_no_python_so_no_bytecode_dir_can_appear() -> None:
    """`roster/` is DATA — one skill.md per agent — and must stay that way.

    DDR-857 briefly put a module here. Importing it created `__pycache__`,
    which the slot test below reads as a ninth agent, and CI went red on
    `934106c`. The module moved to `agents/runtime/roles.py`; this test is what
    stops it (or anything else importable) coming back.

    Asserted at the source rather than by teaching the slot test to skip
    `__pycache__`: an exception there would also hide a real stray directory,
    and the actual rule is that code does not belong in a data directory.
    """
    stray = sorted(p.name for p in ROSTER.rglob("*.py"))
    assert not stray, (
        f"roster/ contains Python: {stray}. It is a data directory; importing "
        "anything from it creates __pycache__, which the slot check below then "
        "reads as an extra agent."
    )


def test_every_kernel_roster_slot_is_covered() -> None:
    """The roster directory must not drift from the kernel's 8 slots."""
    on_disk = {p.name for p in ROSTER.iterdir() if p.is_dir()}
    assert on_disk == set(LEGACY_AGENTS), (
        f"roster/ holds {sorted(on_disk)}, kernel slots are "
        f"{sorted(LEGACY_AGENTS)} — these must not drift apart."
    )


def test_roles_are_unique() -> None:
    """Two agents sharing a role means one of them has no reason to exist."""
    roles = list(LEGACY_AGENTS.values())
    assert len(roles) == len(set(roles)), f"duplicate roles: {roles}"
