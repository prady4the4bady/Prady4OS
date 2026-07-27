"""C-01 — federated knowledge sync.

Gossips **distilled summaries**, never raw weights and never raw user data,
between AETHER instances on a local mesh.

Four gates every inbound packet passes, in this order, because each is cheaper
than the next and the expensive one should not run on a packet that already
failed:

1. **consent** — `FEDERATION_SYNC` must be granted in OOBE (B-11). Without it
   the client does not transmit at all, which is **S2**.
2. **not self** — a node must not ingest its own gossip.
3. **freshness** — older than 300 s is refused, so a captured packet cannot be
   replayed indefinitely.
4. **Merkle** — the payload must hash to the claimed root (**S9**), reusing
   B-03 rather than reimplementing it.

Transport is injected, exactly as in B-12: this module opens no sockets, so
egress stays a declared capability rather than an import side effect. A UDP
transport is supplied for real use and is never constructed by default.
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Callable, Sequence

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.integrity.merkle import merkle_root, verify_payload
from aether.kernel.lockbox.cap_sovereign import CapabilityError, Lockbox

__all__ = [
    "KnowledgePacket",
    "SyncRejected",
    "KnowledgeStore",
    "GossipClient",
    "GossipServer",
    "SYNC_INTERVAL_S",
    "MAX_PACKET_AGE_S",
    "GOSSIP_PORT",
]

DDR_ID = "C-01"
_FILE = "aether/agents/federation/knowledge_sync.py"
CAPABILITY = "CAP_NETWORK_SYNC"

GOSSIP_PORT = 47200
SYNC_INTERVAL_S = 30.0
MAX_PACKET_AGE_S = 300.0
MAX_EMBEDDING_DIMS = 1024

DEFAULT_STORE = Path(__file__).resolve().parent / "knowledge_store.jsonl"


class SyncRejected(PermissionError):
    """An outbound or inbound packet was refused."""


@dataclass(slots=True)
class KnowledgePacket:
    """A distilled summary — never raw weights, never raw user content."""

    origin_id: str
    domain: str
    summary_embedding: tuple[float, ...]
    merkle_root: str = ""
    ts: float = field(default_factory=time.time)

    def leaves(self) -> list[str]:
        """Canonical leaf list the Merkle root is computed over."""
        return [
            f"origin:{self.origin_id}",
            f"domain:{self.domain}",
            f"ts:{self.ts:.6f}",
            *(f"e{i}:{v:.6f}" for i, v in enumerate(self.summary_embedding)),
        ]

    def seal(self) -> KnowledgePacket:
        self.merkle_root = merkle_root(self.leaves())
        return self

    def to_json(self) -> str:
        payload = asdict(self)
        payload["summary_embedding"] = list(self.summary_embedding)
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))

    @classmethod
    def from_json(cls, raw: str) -> KnowledgePacket:
        try:
            rec = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise SyncRejected(f"malformed packet: {exc}") from exc
        try:
            return cls(
                origin_id=str(rec["origin_id"]),
                domain=str(rec["domain"]),
                summary_embedding=tuple(float(v) for v in rec["summary_embedding"]),
                merkle_root=str(rec.get("merkle_root", "")),
                ts=float(rec.get("ts", 0.0)),
            )
        except (KeyError, TypeError, ValueError) as exc:
            raise SyncRejected(f"malformed packet fields: {exc}") from exc


class KnowledgeStore:
    """Append-only store of accepted knowledge."""

    def __init__(self, path: str | Path | None = None) -> None:
        self.path = Path(path) if path is not None else DEFAULT_STORE
        self.path.parent.mkdir(parents=True, exist_ok=True)

    def append(self, packet: KnowledgePacket) -> None:
        with self.path.open("a", encoding="utf-8") as fh:
            fh.write(packet.to_json() + "\n")

    def all(self) -> list[KnowledgePacket]:
        if not self.path.is_file():
            return []
        out: list[KnowledgePacket] = []
        with self.path.open("r", encoding="utf-8") as fh:
            for line in fh:
                stripped = line.strip()
                if stripped:
                    out.append(KnowledgePacket.from_json(stripped))
        return out

    def by_domain(self, domain: str) -> list[KnowledgePacket]:
        return [p for p in self.all() if p.domain == domain]


#: A transport sends one serialised packet to a peer address.
SendFn = Callable[[str, bytes], None]


class GossipServer:
    """Inbound side: validates and stores packets."""

    def __init__(
        self,
        node_id: str,
        lockbox: Lockbox,
        store: KnowledgeStore | None = None,
        audit: AuditLog | None = None,
        max_age_s: float = MAX_PACKET_AGE_S,
        now_fn: Callable[[], float] | None = None,
    ) -> None:
        self.node_id = node_id
        self.lockbox = lockbox
        self.store = store if store is not None else KnowledgeStore()
        self.audit = audit if audit is not None else AuditLog()
        self.max_age_s = float(max_age_s)
        self.now_fn: Callable[[], float] = now_fn if now_fn is not None else time.time

    def accept(self, raw: str | bytes) -> KnowledgePacket:
        """Validate and store one inbound packet, or raise with a reason."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        text = raw.decode("utf-8") if isinstance(raw, bytes) else raw
        packet = KnowledgePacket.from_json(text)

        if packet.origin_id == self.node_id:
            self._reject(packet, "own packet")
        if not packet.origin_id or not packet.domain:
            self._reject(packet, "missing origin or domain")
        if len(packet.summary_embedding) > MAX_EMBEDDING_DIMS:
            self._reject(packet, "embedding too large")

        age = self.now_fn() - packet.ts
        if age > self.max_age_s:
            self._reject(packet, f"stale ({age:.0f}s)")
        if age < -self.max_age_s:
            self._reject(packet, "timestamp in the future")

        if not verify_payload(packet.leaves(), packet.merkle_root):
            self._reject(packet, "merkle verification failed")

        self.store.append(packet)
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="gossip.accept", gate_status="accepted",
            origin=packet.origin_id, domain=packet.domain,
        )
        return packet

    def _reject(self, packet: KnowledgePacket, why: str) -> None:
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="gossip.reject", gate_status="rejected",
            origin=packet.origin_id, reason=why,
        )
        raise SyncRejected(f"packet from {packet.origin_id!r} rejected: {why}")


class GossipClient:
    """Outbound side: seals and broadcasts packets to known peers."""

    def __init__(
        self,
        node_id: str,
        lockbox: Lockbox,
        send_fn: SendFn,
        has_consent: Callable[[], bool],
        audit: AuditLog | None = None,
    ) -> None:
        self.node_id = node_id
        self.lockbox = lockbox
        self.send_fn = send_fn
        self.has_consent = has_consent
        self.audit = audit if audit is not None else AuditLog()
        self._peers: list[str] = []

    def add_peer(self, address: str) -> None:
        if address not in self._peers:
            self._peers.append(address)

    @property
    def peers(self) -> tuple[str, ...]:
        return tuple(self._peers)

    def build(self, domain: str, embedding: Sequence[float]) -> KnowledgePacket:
        if len(embedding) > MAX_EMBEDDING_DIMS:
            raise SyncRejected(
                f"embedding of {len(embedding)} dims exceeds {MAX_EMBEDDING_DIMS}"
            )
        return KnowledgePacket(
            origin_id=self.node_id,
            domain=domain,
            summary_embedding=tuple(float(v) for v in embedding),
        ).seal()

    def broadcast(self, packet: KnowledgePacket) -> int:
        """Send to every known peer. Refuses without consent (S2)."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not self.has_consent():
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="gossip.no_consent",
                gate_status="refused", domain=packet.domain,
            )
            raise SyncRejected(
                "FEDERATION_SYNC consent not granted; refusing to transmit (S2)"
            )
        blob = packet.to_json().encode("utf-8")
        sent = 0
        for address in self._peers:
            try:
                self.send_fn(address, blob)
                sent += 1
            except OSError as exc:
                self.audit.append_action(
                    ddr=DDR_ID, file=_FILE, action="gossip.send_error",
                    gate_status="error", peer=address,
                    error=f"{type(exc).__name__}: {exc}",
                )
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="gossip.broadcast", gate_status="sent",
            domain=packet.domain, peers=sent,
        )
        return sent


def register(lockbox: Lockbox) -> None:
    """Register this DDR's capability with the lockbox."""
    try:
        lockbox.register(DDR_ID, _FILE, CAPABILITY)
    except CapabilityError:  # pragma: no cover - unknown token is a build error
        raise
