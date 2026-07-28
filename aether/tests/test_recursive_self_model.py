"""D-09 discriminating gate — recursive self-model updater."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.agents.self_model.recursive_self_model_updater import (
    CapError,
    RecursiveSelfModelUpdater,
    Revision,
    RevisionKind,
    SelfModelUpdateError,
)
from aether.agents.self_model.self_model import SelfModel
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _setup(tmp_path: Path, granted: bool = True, tap=None):
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("D-09", "recursive_self_model_updater.py", "CAP_SELF_MODEL_WRITE")
    sm = SelfModel(lockbox=lb, store_path=tmp_path / "self_model.jsonl", audit=audit)
    upd = RecursiveSelfModelUpdater(
        lockbox=lb, self_model=sm, audit=audit, alignment_tap=tap
    )
    return upd, sm, lb, audit


def test_self_escalation_via_self_model_raises(tmp_path: Path) -> None:
    """THE discriminator — S1 as a hard invariant.

    Writing "I hold CAP_SOVEREIGN" into the self-model grants that capability
    to every consumer that treats the self-model as ground truth, without ever
    touching the lockbox. The lockbox stays pristine and the audit log shows
    only a routine model update — which is precisely why the check has to live
    here.
    """
    upd, sm, lb, _audit = _setup(tmp_path)
    rev = Revision(
        kind=RevisionKind.ADD_CAPABILITY, name="rule_everything",
        evidence="I observed myself succeeding at everything",
        ddr_id="D-09", capability="CAP_SOVEREIGN",
    )
    with pytest.raises(CapError, match="self-escalation refused"):
        upd.apply(rev)
    assert sm.claims == ()
    assert lb.check("D-09", "CAP_SOVEREIGN") is False


def test_escalation_is_refused_at_proposal_not_only_at_apply(tmp_path: Path) -> None:
    """Refusing only at apply leaves an ungranted capability sitting in a queue
    looking legitimate."""
    upd, _sm, _lb, _audit = _setup(tmp_path)
    rev = Revision(
        kind=RevisionKind.ADD_CAPABILITY, name="x", evidence="e",
        ddr_id="D-09", capability="CAP_NET",
    )
    with pytest.raises(CapError):
        upd.propose(rev)


def test_cap_error_is_the_lockbox_error_type() -> None:
    """A caller already handling capability failures handles escalation too."""
    assert CapError is CapabilityError


def test_granted_capability_is_accepted(tmp_path: Path) -> None:
    """The updater must not be uselessly strict — a real grant goes through."""
    upd, sm, lb, _audit = _setup(tmp_path)
    lb.register("D-01", "self_improvement.py", "CAP_SELF_IMPROVEMENT_PROPOSE")
    result = upd.apply(Revision(
        kind=RevisionKind.ADD_CAPABILITY, name="propose_improvements",
        evidence="lockbox token exists", ddr_id="D-01", capability="CAP_SELF_IMPROVEMENT_PROPOSE",
    ))
    assert result.applied
    assert [c.name for c in sm.claims] == ["propose_improvements"]


def test_escalation_refusal_is_audited(tmp_path: Path) -> None:
    upd, _sm, _lb, _audit = _setup(tmp_path)
    with pytest.raises(CapError):
        upd.apply(Revision(
            kind=RevisionKind.ADD_CAPABILITY, name="x", evidence="e",
            ddr_id="D-09", capability="CAP_SOVEREIGN",
        ))
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "D-09"
               and e["action"] == "self_model.escalation_refused"
               for e in entries)


def test_unevidenced_revision_refused(tmp_path: Path) -> None:
    """S13 — nothing is inferred from an empty history."""
    upd, _sm, lb, _audit = _setup(tmp_path)
    lb.register("D-01", "x.py", "CAP_SELF_IMPROVEMENT_PROPOSE")
    with pytest.raises(SelfModelUpdateError, match="carries no evidence"):
        upd.propose(Revision(
            kind=RevisionKind.ADD_CAPABILITY, name="x", evidence="   ",
            ddr_id="D-01", capability="CAP_SELF_IMPROVEMENT_PROPOSE",
        ))


def test_outcome_below_evidence_floor_refused(tmp_path: Path) -> None:
    """S13 — two observations is noise, not a track record."""
    upd, _sm, _lb, _audit = _setup(tmp_path)
    with pytest.raises(SelfModelUpdateError, match="below the evidence floor"):
        upd.propose(Revision(
            kind=RevisionKind.RECORD_OUTCOME, name="x", evidence="runs",
            successes=1, failures=1,
        ))


def test_negative_counts_refused(tmp_path: Path) -> None:
    upd, _sm, _lb, _audit = _setup(tmp_path)
    with pytest.raises(SelfModelUpdateError, match="must be >= 0"):
        upd.propose(Revision(
            kind=RevisionKind.RECORD_OUTCOME, name="x", evidence="e",
            successes=-5, failures=10,
        ))


def test_outcomes_move_confidence_by_evidence(tmp_path: Path) -> None:
    upd, sm, lb, _audit = _setup(tmp_path)
    lb.register("D-01", "x.py", "CAP_SELF_IMPROVEMENT_PROPOSE")
    upd.apply(Revision(
        kind=RevisionKind.ADD_CAPABILITY, name="propose", evidence="granted",
        ddr_id="D-01", capability="CAP_SELF_IMPROVEMENT_PROPOSE",
    ))
    upd.apply(Revision(
        kind=RevisionKind.RECORD_OUTCOME, name="propose", evidence="4 runs",
        successes=3, failures=1,
    ))
    assert sm.confidence("propose") == pytest.approx(0.75)
    assert sm.can("propose") is True


def test_batch_stops_at_the_first_escalation(tmp_path: Path) -> None:
    """A batch containing an escalation attempt is not partially trustworthy."""
    upd, sm, lb, _audit = _setup(tmp_path)
    lb.register("D-01", "x.py", "CAP_SELF_IMPROVEMENT_PROPOSE")
    good = Revision(kind=RevisionKind.ADD_CAPABILITY, name="ok", evidence="e",
                    ddr_id="D-01", capability="CAP_SELF_IMPROVEMENT_PROPOSE")
    bad = Revision(kind=RevisionKind.ADD_CAPABILITY, name="bad", evidence="e",
                   ddr_id="D-09", capability="CAP_SOVEREIGN")
    after = Revision(kind=RevisionKind.RECORD_LIMIT, name="never", evidence="e")
    with pytest.raises(CapError):
        upd.apply_many([good, bad, after])
    names = [c.name for c in sm.claims]
    assert names == ["ok"]
    assert [l.description for l in sm.limits] == []


def test_reconcile_drops_claims_the_lockbox_no_longer_backs(tmp_path: Path) -> None:
    """The recursive half: a stale claim makes the system keep attempting what
    it can no longer do and read the failures as bad luck."""
    upd, sm, lb, audit = _setup(tmp_path)
    lb.register("D-01", "x.py", "CAP_SELF_IMPROVEMENT_PROPOSE")
    upd.apply(Revision(
        kind=RevisionKind.ADD_CAPABILITY, name="propose", evidence="granted",
        ddr_id="D-01", capability="CAP_SELF_IMPROVEMENT_PROPOSE",
    ))
    assert upd.reconcile() == []                 # nothing stale yet

    # A lockbox reloaded without that grant — the capability is gone.
    stricter = Lockbox(registry_path=tmp_path / "other_tokens.jsonl", audit=audit)
    stricter.register("D-09", "recursive_self_model_updater.py",
                      "CAP_SELF_MODEL_WRITE")
    upd.lockbox = stricter
    stale = upd.reconcile()
    assert len(stale) == 1
    assert stale[0].revision.name == "propose"
    assert any("no longer granted" in l.evidence for l in sm.limits)


def test_limit_revision_applies(tmp_path: Path) -> None:
    upd, sm, _lb, _audit = _setup(tmp_path)
    upd.apply(Revision(kind=RevisionKind.RECORD_LIMIT, name="cannot fly",
                       evidence="10 failed attempts"))
    assert [l.description for l in sm.limits] == ["cannot fly"]


def test_missing_name_refused(tmp_path: Path) -> None:
    upd, _sm, _lb, _audit = _setup(tmp_path)
    with pytest.raises(SelfModelUpdateError, match="needs a target name"):
        upd.propose(Revision(kind=RevisionKind.RECORD_LIMIT, name="", evidence="e"))


def test_capability_revision_needs_ddr_and_capability(tmp_path: Path) -> None:
    upd, _sm, _lb, _audit = _setup(tmp_path)
    with pytest.raises(SelfModelUpdateError, match="needs ddr_id and capability"):
        upd.propose(Revision(kind=RevisionKind.ADD_CAPABILITY, name="x",
                             evidence="e"))


def test_capability_required(tmp_path: Path) -> None:
    upd, _sm, _lb, _audit = _setup(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_SELF_MODEL_WRITE"):
        upd.propose(Revision(kind=RevisionKind.RECORD_LIMIT, name="x", evidence="e"))


def test_alignment_tap_receives_steps(tmp_path: Path) -> None:
    events: list[str] = []
    upd, _sm, lb, _audit = _setup(tmp_path, tap=lambda s, _d: events.append(s))
    lb.register("D-01", "x.py", "CAP_SELF_IMPROVEMENT_PROPOSE")
    upd.apply(Revision(kind=RevisionKind.ADD_CAPABILITY, name="ok", evidence="e",
                       ddr_id="D-01", capability="CAP_SELF_IMPROVEMENT_PROPOSE"))
    upd.reconcile()
    with pytest.raises(CapError):
        upd.apply(Revision(kind=RevisionKind.ADD_CAPABILITY, name="bad",
                           evidence="e", ddr_id="D-09", capability="CAP_SOVEREIGN"))
    assert {"D-09.propose", "D-09.apply", "D-09.reconcile",
            "D-09.escalation_refused"} <= set(events)


def test_history_records_applied_revisions(tmp_path: Path) -> None:
    upd, _sm, _lb, _audit = _setup(tmp_path)
    upd.apply(Revision(kind=RevisionKind.RECORD_LIMIT, name="l", evidence="e"))
    assert len(upd.history) == 1
    assert upd.history[0].applied
