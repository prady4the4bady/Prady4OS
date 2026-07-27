"""C-01 discriminating gate — federated knowledge sync."""

from __future__ import annotations

import time
from pathlib import Path

import pytest

from aether.agents.federation.knowledge_sync import (
    MAX_PACKET_AGE_S,
    GossipClient,
    GossipServer,
    KnowledgePacket,
    KnowledgeStore,
    SyncRejected,
    register,
)
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox


def _lockbox(tmp_path: Path, granted: bool = True) -> Lockbox:
    lb = Lockbox(
        registry_path=tmp_path / "tokens.jsonl",
        audit=AuditLog(tmp_path / "audit_log.jsonl"),
    )
    if granted:
        register(lb)
    return lb


def _node(tmp_path: Path, name: str, consent: bool = True):
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = _lockbox(tmp_path)
    store = KnowledgeStore(tmp_path / f"{name}_store.jsonl")
    server = GossipServer(node_id=name, lockbox=lb, store=store, audit=audit)
    inbox: list[bytes] = []
    client = GossipClient(
        node_id=name,
        lockbox=lb,
        send_fn=lambda _addr, blob: inbox.append(blob),
        has_consent=lambda: consent,
        audit=audit,
    )
    return client, server, store, inbox


def test_gossip_roundtrip(tmp_path: Path) -> None:
    """The mandated gate: two instances exchange a packet, both stores written."""
    a_client, _a_server, a_store, wire = _node(tmp_path, "node-a")
    _b_client, b_server, b_store, _ = _node(tmp_path, "node-b")

    a_client.add_peer("node-b:47200")
    packet = a_client.build("logistics", [0.1, 0.2, 0.3])
    assert a_client.broadcast(packet) == 1

    # A keeps its own summary; B ingests the gossiped one.
    a_store.append(packet)
    received = b_server.accept(wire[0])

    assert received.origin_id == "node-a"
    assert received.domain == "logistics"
    assert len(a_store.all()) == 1
    assert len(b_store.all()) == 1
    assert b_store.all()[0].summary_embedding == (0.1, 0.2, 0.3)


def test_tampered_payload_refused(tmp_path: Path) -> None:
    """S9 — the discriminator.

    A packet whose embedding was altered in flight still carries the honest
    root. Only re-deriving the root catches it; trusting the claimed root
    accepts poisoned knowledge.
    """
    a_client, _s, _st, wire = _node(tmp_path, "node-a")
    _c, b_server, _bs, _ = _node(tmp_path, "node-b")
    a_client.add_peer("peer")
    a_client.broadcast(a_client.build("logistics", [0.1, 0.2]))

    honest = KnowledgePacket.from_json(wire[0].decode())
    tampered = KnowledgePacket(
        origin_id=honest.origin_id,
        domain=honest.domain,
        summary_embedding=(9.9, 9.9),          # poisoned
        merkle_root=honest.merkle_root,        # honest root retained
        ts=honest.ts,
    )
    with pytest.raises(SyncRejected, match="merkle verification failed"):
        b_server.accept(tampered.to_json())


def test_without_consent_nothing_is_transmitted(tmp_path: Path) -> None:
    """S2 — no egress without explicit consent, checked before any send."""
    client, _s, _st, wire = _node(tmp_path, "node-a", consent=False)
    client.add_peer("peer")
    with pytest.raises(SyncRejected, match="consent not granted"):
        client.broadcast(client.build("logistics", [0.1]))
    assert wire == [], "packet left the node without consent"


def test_capability_is_required(tmp_path: Path) -> None:
    """S6 — an ungranted node cannot sync at all."""
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = _lockbox(tmp_path, granted=False)
    server = GossipServer(
        node_id="n", lockbox=lb, store=KnowledgeStore(tmp_path / "s.jsonl"), audit=audit
    )
    packet = KnowledgePacket(origin_id="other", domain="d", summary_embedding=(0.1,)).seal()
    with pytest.raises(CapabilityError, match="CAP_NETWORK_SYNC"):
        server.accept(packet.to_json())


def test_own_packet_refused(tmp_path: Path) -> None:
    _c, server, _st, _w = _node(tmp_path, "node-a")
    mine = KnowledgePacket(origin_id="node-a", domain="d", summary_embedding=(0.1,)).seal()
    with pytest.raises(SyncRejected, match="own packet"):
        server.accept(mine.to_json())


def test_stale_packet_refused(tmp_path: Path) -> None:
    """A captured packet must not be replayable indefinitely."""
    _c, server, _st, _w = _node(tmp_path, "node-a")
    old = KnowledgePacket(
        origin_id="node-b", domain="d", summary_embedding=(0.1,),
        ts=time.time() - (MAX_PACKET_AGE_S + 60),
    ).seal()
    with pytest.raises(SyncRejected, match="stale"):
        server.accept(old.to_json())


def test_future_dated_packet_refused(tmp_path: Path) -> None:
    _c, server, _st, _w = _node(tmp_path, "node-a")
    ahead = KnowledgePacket(
        origin_id="node-b", domain="d", summary_embedding=(0.1,),
        ts=time.time() + (MAX_PACKET_AGE_S * 2),
    ).seal()
    with pytest.raises(SyncRejected, match="future"):
        server.accept(ahead.to_json())


def test_malformed_packet_refused(tmp_path: Path) -> None:
    _c, server, _st, _w = _node(tmp_path, "node-a")
    with pytest.raises(SyncRejected, match="malformed packet"):
        server.accept("{not json")
    with pytest.raises(SyncRejected, match="malformed packet fields"):
        server.accept('{"origin_id": "x"}')


def test_oversized_embedding_refused(tmp_path: Path) -> None:
    client, _s, _st, _w = _node(tmp_path, "node-a")
    with pytest.raises(SyncRejected, match="exceeds"):
        client.build("d", [0.1] * 2000)


def test_module_opens_no_sockets(tmp_path: Path) -> None:
    """Transport is injected; egress stays a declared capability."""
    client, server, _st, _w = _node(tmp_path, "node-a")
    for forbidden in ("connect", "bind", "listen", "socket", "serve_forever"):
        assert not hasattr(client, forbidden)
        assert not hasattr(server, forbidden)


def test_store_is_append_only_and_queryable(tmp_path: Path) -> None:
    store = KnowledgeStore(tmp_path / "k.jsonl")
    store.append(KnowledgePacket("a", "alpha", (0.1,)).seal())
    store.append(KnowledgePacket("b", "beta", (0.2,)).seal())
    assert len(store.all()) == 2
    assert [p.origin_id for p in store.by_domain("beta")] == ["b"]
    for forbidden in ("delete", "update", "clear", "remove"):
        assert not hasattr(store, forbidden)


def test_rejections_are_audited(tmp_path: Path) -> None:
    audit_path = tmp_path / "audit_log.jsonl"
    lb = _lockbox(tmp_path)
    server = GossipServer(
        node_id="node-a", lockbox=lb,
        store=KnowledgeStore(tmp_path / "s.jsonl"), audit=AuditLog(audit_path),
    )
    bad = KnowledgePacket("node-b", "d", (0.1,), merkle_root="deadbeef")
    with pytest.raises(SyncRejected):
        server.accept(bad.to_json())
    entries = AuditLog(audit_path).read_all()
    assert any(e["ddr"] == "C-01" and e["action"] == "gossip.reject" for e in entries)


def test_send_failure_does_not_abort_broadcast(tmp_path: Path) -> None:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = _lockbox(tmp_path)
    delivered: list[str] = []

    def flaky(address: str, _blob: bytes) -> None:
        if address == "bad":
            raise OSError("unreachable")
        delivered.append(address)

    client = GossipClient(
        node_id="n", lockbox=lb, send_fn=flaky, has_consent=lambda: True, audit=audit
    )
    for peer in ("good-1", "bad", "good-2"):
        client.add_peer(peer)
    assert client.broadcast(client.build("d", [0.1])) == 2
    assert delivered == ["good-1", "good-2"]
