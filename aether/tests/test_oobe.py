"""B-11 discriminating gate — out-of-box experience."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.kernel.audit.audit_log import AuditLog
from aether.platform.oobe.oobe import Consent, OOBE, OOBEError


def _oobe(tmp_path: Path) -> OOBE:
    return OOBE(
        state_path=tmp_path / "oobe_state.json",
        audit=AuditLog(tmp_path / "audit_log.jsonl"),
    )


def test_every_consent_defaults_denied(tmp_path: Path) -> None:
    """S2 — the discriminator.

    Nothing may be permitted by default. A flow that pre-ticks even one consent
    (telemetry is the usual one) fails here.
    """
    o = _oobe(tmp_path)
    for consent in Consent:
        assert o.has_consent(consent) is False, f"{consent} was granted by default"


def test_no_bulk_grant_api(tmp_path: Path) -> None:
    """Consent must be named one at a time — no sweep."""
    o = _oobe(tmp_path)
    for forbidden in ("grant_all", "accept_all", "grant_many", "allow_all"):
        assert not hasattr(o, forbidden)


def test_grant_requires_a_consent_member(tmp_path: Path) -> None:
    o = _oobe(tmp_path)
    with pytest.raises(TypeError):
        o.grant("network_egress")  # type: ignore[arg-type]


def test_happy_path(tmp_path: Path) -> None:
    o = _oobe(tmp_path)
    o.set_operator("prady", "Prady")
    o.choose_model("llama3")
    o.grant(Consent.FEDERATION_SYNC)
    state = o.complete()
    assert state.completed is True
    assert o.has_consent(Consent.FEDERATION_SYNC) is True
    assert o.has_consent(Consent.TELEMETRY) is False


def test_cannot_complete_without_operator_or_model(tmp_path: Path) -> None:
    o = _oobe(tmp_path)
    with pytest.raises(OOBEError, match="operator profile"):
        o.complete()
    o.set_operator("prady")
    with pytest.raises(OOBEError, match="choosing a model"):
        o.complete()


def test_completed_setup_is_immutable(tmp_path: Path) -> None:
    """A re-runnable setup flow is an escalation path."""
    o = _oobe(tmp_path)
    o.set_operator("prady")
    o.choose_model("llama3")
    o.complete()
    with pytest.raises(OOBEError, match="already completed"):
        o.grant(Consent.NETWORK_EGRESS)
    with pytest.raises(OOBEError, match="already completed"):
        o.set_operator("attacker")
    with pytest.raises(OOBEError, match="already completed"):
        o.complete()


def test_rerun_does_not_reset_state(tmp_path: Path) -> None:
    """Constructing a fresh OOBE over a completed state must not reopen it."""
    o = _oobe(tmp_path)
    o.set_operator("prady")
    o.choose_model("llama3")
    o.complete()

    reborn = _oobe(tmp_path)
    assert reborn.completed is True
    assert reborn.state.profile is not None
    assert reborn.state.profile.operator_id == "prady"
    with pytest.raises(OOBEError):
        reborn.grant(Consent.TELEMETRY)


def test_revise_is_attributed_and_audited(tmp_path: Path) -> None:
    audit_path = tmp_path / "audit_log.jsonl"
    o = OOBE(state_path=tmp_path / "s.json", audit=AuditLog(audit_path))
    o.set_operator("prady")
    o.choose_model("llama3")
    o.complete()

    o.revise(Consent.NETWORK_EGRESS, True, changed_by="prady", reason="enable sync")
    assert o.has_consent(Consent.NETWORK_EGRESS) is True

    entries = AuditLog(audit_path).read_all()
    revisions = [e for e in entries if e["action"] == "oobe.revise"]
    assert revisions
    assert revisions[0]["extra"]["changed_by"] == "prady"
    assert revisions[0]["extra"]["reason"] == "enable sync"


def test_revise_demands_attribution(tmp_path: Path) -> None:
    o = _oobe(tmp_path)
    o.set_operator("prady")
    o.choose_model("llama3")
    o.complete()
    with pytest.raises(ValueError, match="changed_by and reason"):
        o.revise(Consent.TELEMETRY, True, changed_by="", reason="")


def test_revise_rejected_before_completion(tmp_path: Path) -> None:
    o = _oobe(tmp_path)
    with pytest.raises(OOBEError, match="completed setups"):
        o.revise(Consent.TELEMETRY, True, changed_by="x", reason="y")


def test_revise_can_revoke_not_only_grant(tmp_path: Path) -> None:
    """An implementation that ignores `value` and always grants passes every
    other revise test here — and turns the one post-setup lever the operator
    has for taking permission BACK into a lever that only ever widens it.
    """
    o = _oobe(tmp_path)
    o.set_operator("prady")
    o.choose_model("llama3")
    o.grant(Consent.FEDERATION_SYNC)
    o.complete()
    assert o.has_consent(Consent.FEDERATION_SYNC) is True

    o.revise(Consent.FEDERATION_SYNC, False, changed_by="prady", reason="revoking")
    assert o.has_consent(Consent.FEDERATION_SYNC) is False

    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    revocations = [e for e in entries
                   if e["action"] == "oobe.revise" and e["gate_status"] == "revoked"]
    assert revocations, "a revocation was recorded as a grant"


def test_granted_consent_survives_a_restart(tmp_path: Path) -> None:
    """Consent that silently resets on restart is consent the operator never
    actually gave the second time — and nothing would surface the difference."""
    o = _oobe(tmp_path)
    o.set_operator("prady")
    o.choose_model("llama3")
    o.grant(Consent.FEDERATION_SYNC)
    o.complete()

    reborn = _oobe(tmp_path)
    assert reborn.has_consent(Consent.FEDERATION_SYNC) is True
    assert reborn.has_consent(Consent.TELEMETRY) is False   # and no widening


def test_revocation_survives_a_restart(tmp_path: Path) -> None:
    """The direction that actually matters: a revoked consent must not come
    back on the next boot."""
    o = _oobe(tmp_path)
    o.set_operator("prady")
    o.choose_model("llama3")
    o.grant(Consent.FEDERATION_SYNC)
    o.complete()
    o.revise(Consent.FEDERATION_SYNC, False, changed_by="prady", reason="revoking")

    assert _oobe(tmp_path).has_consent(Consent.FEDERATION_SYNC) is False


def test_corrupt_state_reported(tmp_path: Path) -> None:
    path = tmp_path / "oobe_state.json"
    path.write_text("{not json", encoding="utf-8")
    with pytest.raises(OOBEError, match="corrupt OOBE state"):
        OOBE(state_path=path, audit=AuditLog(tmp_path / "a.jsonl"))
