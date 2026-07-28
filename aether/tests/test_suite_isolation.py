"""QUIRK-10 gate — the suite must not leak state or write into the repo.

These two tests are ordered deliberately: the first grants a capability on the
process-default lockbox, the second asserts it is gone. Without the autouse
fixture in conftest.py the grant survives into the second test (and into every
later module), which is the exact "passes in isolation, fails in suite" shape —
and worse, it can make a test that *should* fail on a missing capability pass.
"""

from __future__ import annotations

from pathlib import Path

from aether.kernel.audit import audit_log as audit_mod
from aether.kernel.lockbox import cap_sovereign as lockbox_mod

_REPO_AUDIT = Path(audit_mod.__file__).resolve().parent / "audit_log.jsonl"
_REPO_TOKENS = Path(lockbox_mod.__file__).resolve().parent / "tokens.jsonl"

_LEAK_DDR = "QUIRK10-LEAK-PROBE"


def test_a_grant_on_the_default_lockbox() -> None:
    """Dirty the process-default singletons on purpose."""
    lockbox_mod.register(_LEAK_DDR, "leak_probe.py", "CAP_WATCHDOG")
    assert lockbox_mod.check(_LEAK_DDR, "CAP_WATCHDOG") is True
    audit_mod.append(
        audit_mod.AuditRecord(
            ddr=_LEAK_DDR, file="leak_probe.py", action="leak", gate_status="probe"
        )
    )
    assert any(r["ddr"] == _LEAK_DDR for r in audit_mod.read_all())


def test_b_previous_test_state_did_not_leak() -> None:
    """The discriminator: without conftest isolation this FAILS."""
    assert lockbox_mod.check(_LEAK_DDR, "CAP_WATCHDOG") is False, (
        "lockbox state leaked from a previous test — conftest isolation is not active"
    )
    assert not any(r["ddr"] == _LEAK_DDR for r in audit_mod.read_all()), (
        "audit state leaked from a previous test"
    )


def test_defaults_do_not_point_into_the_repo() -> None:
    """Running the suite must not write into the source tree."""
    assert _REPO_AUDIT not in {audit_mod._DEFAULT_LOG.path}
    assert _REPO_TOKENS not in {lockbox_mod._DEFAULT_LOCKBOX.path}
    assert not _REPO_AUDIT.exists(), f"suite wrote into the repo: {_REPO_AUDIT}"
    assert not _REPO_TOKENS.exists(), f"suite wrote into the repo: {_REPO_TOKENS}"
