"""C-09 discriminating gate — versioned agent contracts."""

from __future__ import annotations

from pathlib import Path

import pytest

from aether.agents.agentnet.mesh import Mesh, MeshRejected, PeerContract
from aether.agents.contracts.agent_contract import (
    AgentContract,
    ContractError,
    ContractRegistry,
    contract_for,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _setup(tmp_path: Path, granted: bool = True) -> tuple[Lockbox, ContractRegistry]:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    if granted:
        lb.register("C-09", "agent_contract.py", "CAP_CONTRACT_WRITE")
    lb.register("D-10", "watchdog.py", "CAP_WATCHDOG")
    reg = ContractRegistry(
        lockbox=lb, store_path=tmp_path / "registry.jsonl", audit=audit
    )
    return lb, reg


def _valid(lb: Lockbox) -> AgentContract:
    return contract_for(
        agent_id="D-10", ddr_id="D-10", capability="CAP_WATCHDOG",
        lockbox=lb, invariants=("S11",),
        input_schema={"agent_id": "str"}, output_schema={"status": "str"},
    )


def test_contract_verify(tmp_path: Path) -> None:
    """The mandated gate: valid contract verifies; a mutated field is caught."""
    lb, reg = _setup(tmp_path)
    contract = reg.register(_valid(lb))
    valid, violations = reg.verify("D-10")
    assert valid is True and violations == []

    contract.version = "9.9.9"                   # mutate after registration
    valid, violations = reg.verify("D-10")
    assert valid is False
    assert any("fingerprint changed" in v for v in violations)


def test_claimed_capability_must_be_granted(tmp_path: Path) -> None:
    """The discriminator.

    Without the lockbox cross-check a contract is a self-signed assertion of
    authority — S6 inverted. This contract is well-formed in every other way.
    """
    lb, reg = _setup(tmp_path)
    overclaim = AgentContract(
        agent_id="D-13", ddr_id="D-13", version="1.0.0",
        capabilities=("CAP_SOVEREIGN",),          # never granted
    )
    with pytest.raises(ContractError, match="not granted by the lockbox"):
        reg.register(overclaim)
    assert reg.get("D-13") is None


def test_unknown_capability_refused(tmp_path: Path) -> None:
    _lb, reg = _setup(tmp_path)
    bogus = AgentContract(
        agent_id="X", ddr_id="X", version="1.0.0", capabilities=("CAP_INVENTED",)
    )
    with pytest.raises(ContractError, match="unknown capabilities"):
        reg.register(bogus)


def test_pledged_invariants_must_exist(tmp_path: Path) -> None:
    """A pledge to a non-existent invariant reads as a promise but binds nothing."""
    lb, reg = _setup(tmp_path)
    contract = _valid(lb)
    contract.invariants_pledged = ("S11", "S99")
    with pytest.raises(ContractError, match="unknown invariants pledged"):
        reg.register(contract)


def test_required_fields(tmp_path: Path) -> None:
    _lb, reg = _setup(tmp_path)
    with pytest.raises(ContractError, match="version is empty"):
        reg.register(AgentContract(agent_id="A", ddr_id="A", version=""))


def test_unregistered_agent_does_not_verify(tmp_path: Path) -> None:
    _lb, reg = _setup(tmp_path)
    valid, violations = reg.verify("nobody")
    assert valid is False
    assert "no contract registered" in violations[0]


def test_registry_replaces_the_mesh_reject_all_default(tmp_path: Path) -> None:
    """C-09 is what makes B-12's mesh usable without weakening S11."""
    lb, reg = _setup(tmp_path)
    reg.register(_valid(lb))
    audit = AuditLog(tmp_path / "audit_log.jsonl")

    mesh = Mesh(self_id="node-self", verifier=reg.verify_contract, audit=audit)
    peer = PeerContract(
        agent_id="D-10", ddr_id="D-10", version="1.0.0",
        capabilities=("CAP_WATCHDOG",), invariants_pledged=("S11",),
    )
    # The mesh's PeerContract mirrors the registered AgentContract's surface.
    joined = mesh.join(
        "D-10", "key-a",
        PeerContract(
            agent_id=peer.agent_id, ddr_id=peer.ddr_id, version=peer.version,
            capabilities=peer.capabilities, invariants_pledged=peer.invariants_pledged,
        ),
    )
    assert joined.peer_id == "D-10"


def test_mesh_still_refuses_an_overclaiming_peer(tmp_path: Path) -> None:
    lb, reg = _setup(tmp_path)
    reg.register(_valid(lb))
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    mesh = Mesh(self_id="node-self", verifier=reg.verify_contract, audit=audit)
    with pytest.raises(MeshRejected, match="contract verification failed"):
        mesh.join(
            "D-99", "key-z",
            PeerContract(
                agent_id="D-99", ddr_id="D-99", version="1.0.0",
                capabilities=("CAP_SOVEREIGN",),
            ),
        )


def test_presented_contract_must_match_the_registered_one(tmp_path: Path) -> None:
    """A peer must not present a wider contract than the one on file."""
    lb, reg = _setup(tmp_path)
    reg.register(_valid(lb))
    widened = _valid(lb)
    widened.output_schema = {"status": "str", "secret": "str"}
    assert reg.verify_contract(widened) is False


def test_capability_required_to_register(tmp_path: Path) -> None:
    lb, reg = _setup(tmp_path, granted=False)
    with pytest.raises(CapabilityError, match="CAP_CONTRACT_WRITE"):
        reg.register(_valid(lb))


def test_contracts_survive_restart(tmp_path: Path) -> None:
    lb, reg = _setup(tmp_path)
    reg.register(_valid(lb))
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    reborn = ContractRegistry(
        lockbox=lb, store_path=tmp_path / "registry.jsonl", audit=audit
    )
    valid, violations = reborn.verify("D-10")
    assert valid is True, violations
    assert reborn.pledges("D-10") == ("S11",)


def test_fingerprint_ignores_timestamp(tmp_path: Path) -> None:
    """Two contracts with the same surface must fingerprint identically."""
    lb, _reg = _setup(tmp_path)
    a = _valid(lb)
    b = _valid(lb)
    b.ts = a.ts + 5000.0
    assert a.fingerprint() == b.fingerprint()


def test_rejections_are_audited(tmp_path: Path) -> None:
    _lb, reg = _setup(tmp_path)
    with pytest.raises(ContractError):
        reg.register(
            AgentContract(agent_id="X", ddr_id="X", version="1", capabilities=("CAP_NOPE",))
        )
    entries = AuditLog(tmp_path / "audit_log.jsonl").read_all()
    assert any(e["ddr"] == "C-09" and e["action"] == "contract.rejected" for e in entries)
