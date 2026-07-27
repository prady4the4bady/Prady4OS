"""B-12 discriminating gate — AgentNet P2P mesh."""

from __future__ import annotations

import time
from pathlib import Path

import pytest

from aether.agents.agentnet.mesh import (
    Mesh,
    MeshMessage,
    MeshRejected,
    PeerContract,
    reject_all_verifier,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.integrity.merkle import merkle_root


def _contract(peer_id: str) -> PeerContract:
    return PeerContract(
        agent_id=peer_id,
        ddr_id="C-01",
        version="1.0.0",
        capabilities=("CAP_NETWORK_SYNC",),
        invariants_pledged=("S9",),
    )


def _mesh(tmp_path: Path, verifier=lambda _c: True) -> Mesh:
    return Mesh(
        self_id="node-self",
        verifier=verifier,
        audit=AuditLog(tmp_path / "audit_log.jsonl"),
    )


def test_join_and_list(tmp_path: Path) -> None:
    m = _mesh(tmp_path)
    m.join("node-a", "key-a", _contract("node-a"))
    assert m.peer_ids == ("node-a",)
    assert m.contract_of("node-a") is not None


def test_default_verifier_refuses_everything(tmp_path: Path) -> None:
    """S11 — the discriminator.

    If a deployment forgets to wire the contract registry, the mesh must stay
    CLOSED. A permissive default would silently trust whoever connects, which
    is precisely what S11 forbids.
    """
    m = Mesh(self_id="node-self", audit=AuditLog(tmp_path / "a.jsonl"))
    assert m.verifier is reject_all_verifier
    with pytest.raises(MeshRejected, match="contract verification failed"):
        m.join("node-a", "key-a", _contract("node-a"))
    assert m.peer_ids == ()


def test_join_refused_when_contract_id_mismatches(tmp_path: Path) -> None:
    m = _mesh(tmp_path)
    with pytest.raises(MeshRejected, match="contract agent_id"):
        m.join("node-a", "key-a", _contract("someone-else"))


def test_impersonation_refused(tmp_path: Path) -> None:
    """S5 — an id belongs to the key that first claimed it."""
    m = _mesh(tmp_path)
    m.join("node-a", "key-a", _contract("node-a"))
    m.join("node-a", "key-a", _contract("node-a"))          # same key: fine
    with pytest.raises(MeshRejected, match="already bound to a different key"):
        m.join("node-a", "key-EVIL", _contract("node-a"))


def test_cannot_join_or_message_self(tmp_path: Path) -> None:
    m = _mesh(tmp_path)
    with pytest.raises(MeshRejected, match="cannot join itself"):
        m.join("node-self", "k", _contract("node-self"))
    payload = ["x"]
    msg = MeshMessage(
        origin_id="node-self", topic="t", payload=payload,
        merkle_root=merkle_root(payload),
    )
    with pytest.raises(MeshRejected, match="from self"):
        m.accept(msg)


def test_message_from_unknown_peer_refused(tmp_path: Path) -> None:
    m = _mesh(tmp_path)
    payload = ["hello"]
    msg = MeshMessage(
        origin_id="stranger", topic="t", payload=payload,
        merkle_root=merkle_root(payload),
    )
    with pytest.raises(MeshRejected, match="unknown peer"):
        m.accept(msg)


def test_tampered_payload_refused(tmp_path: Path) -> None:
    """S9 — the root must match the payload actually delivered."""
    m = _mesh(tmp_path)
    m.join("node-a", "key-a", _contract("node-a"))
    honest = ["alpha", "beta"]
    msg = MeshMessage(
        origin_id="node-a", topic="t", payload=honest,
        merkle_root=merkle_root(honest),
    )
    m.accept(msg)                                    # unmodified: accepted

    tampered = MeshMessage(
        origin_id="node-a", topic="t", payload=["alpha", "EVIL"],
        merkle_root=merkle_root(honest),             # claims the honest root
    )
    with pytest.raises(MeshRejected, match="merkle verification failed"):
        m.accept(tampered)


def test_stale_message_refused(tmp_path: Path) -> None:
    m = _mesh(tmp_path)
    m.join("node-a", "key-a", _contract("node-a"))
    payload = ["x"]
    old = MeshMessage(
        origin_id="node-a", topic="t", payload=payload,
        merkle_root=merkle_root(payload), ts=time.time() - 3600,
    )
    with pytest.raises(MeshRejected, match="too old"):
        m.accept(old)


def test_signature_roundtrip_and_forgery(tmp_path: Path) -> None:
    m = _mesh(tmp_path)
    m.join("node-a", "key-a", _contract("node-a"))
    payload = ["signed"]
    msg = MeshMessage(
        origin_id="node-a", topic="t", payload=payload,
        merkle_root=merkle_root(payload),
    )
    m.sign(msg, secret="shared-secret")
    assert m.accept(msg, secret="shared-secret") is msg

    with pytest.raises(MeshRejected, match="signature verification failed"):
        m.accept(msg, secret="wrong-secret")


def test_leave_removes_peer(tmp_path: Path) -> None:
    m = _mesh(tmp_path)
    m.join("node-a", "key-a", _contract("node-a"))
    m.leave("node-a")
    assert m.peer_ids == ()
    assert m.contract_of("node-a") is None


def test_refusals_are_audited(tmp_path: Path) -> None:
    audit_path = tmp_path / "audit_log.jsonl"
    m = Mesh(self_id="node-self", verifier=lambda _c: True, audit=AuditLog(audit_path))
    m.join("node-a", "key-a", _contract("node-a"))
    with pytest.raises(MeshRejected):
        m.join("node-a", "key-EVIL", _contract("node-a"))
    entries = AuditLog(audit_path).read_all()
    assert any(e["action"] == "mesh.impersonation_refused" for e in entries)


def test_mesh_opens_no_sockets(tmp_path: Path) -> None:
    """Transport is injected; egress must be a declared capability, not a
    side effect of constructing the mesh."""
    m = _mesh(tmp_path)
    for forbidden in ("connect", "listen", "serve", "bind", "socket"):
        assert not hasattr(m, forbidden)
