"""B-12 — AgentNet P2P mesh.

Peer discovery and handshake between AETHER instances. This is the layer's
trust boundary, so the rules are stricter than anything internal:

* **S11 — never run an agent without a registered, verified contract.** A join
  is refused unless the peer presents a contract that verifies. C-09 supplies
  the real registry later; the verifier is injected here so the check can never
  become optional, and the *default* verifier refuses everything.
* **S5 — no impersonation.** A peer id is bound to the public key that first
  claimed it. A second peer presenting the same id with a different key is
  refused, and the attempt is audited.
* **S9 — Merkle-verified payloads.** Messages carry a Merkle root over their
  payload; :meth:`Mesh.accept` rejects anything that does not verify, so C-01's
  gossip inherits the check rather than reimplementing it.

Transport is injected. The mesh does not open sockets — that keeps it testable
and keeps egress a declared capability (B-11 ``NETWORK_EGRESS``) rather than a
side effect of importing a module.
"""

from __future__ import annotations

import hashlib
import hmac
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Sequence

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.integrity.merkle import merkle_root, verify_payload

__all__ = [
    "PeerIdentity",
    "PeerContract",
    "MeshMessage",
    "Mesh",
    "MeshRejected",
    "ContractVerifier",
    "reject_all_verifier",
]

DDR_ID = "B-12"
_FILE = "aether/agents/agentnet/mesh.py"

MAX_MESSAGE_AGE_S = 300.0


class MeshRejected(PermissionError):
    """A join or message was refused."""


@dataclass(slots=True, frozen=True)
class PeerContract:
    """What a peer claims about itself at join time."""

    agent_id: str
    ddr_id: str
    version: str
    capabilities: tuple[str, ...] = ()
    invariants_pledged: tuple[str, ...] = ()


@dataclass(slots=True, frozen=True)
class PeerIdentity:
    peer_id: str
    public_key: str          # opaque; an HMAC shared secret id in this build
    joined_ts: float = 0.0


@dataclass(slots=True)
class MeshMessage:
    origin_id: str
    topic: str
    payload: Sequence[str]
    merkle_root: str
    ts: float = field(default_factory=time.time)
    signature: str = ""

    def compute_root(self) -> str:
        return merkle_root(list(self.payload))


#: A verifier answers: is this contract acceptable? Default: no.
ContractVerifier = Callable[[PeerContract], bool]


def reject_all_verifier(_contract: PeerContract) -> bool:
    """Default verifier — refuses every contract.

    Deliberately hostile: if a deployment forgets to wire C-09, the mesh stays
    closed instead of silently trusting whoever connects.
    """
    return False


class Mesh:
    """Peer registry + message admission for the AgentNet mesh."""

    def __init__(
        self,
        self_id: str,
        verifier: ContractVerifier | None = None,
        audit: AuditLog | None = None,
        max_age_s: float = MAX_MESSAGE_AGE_S,
    ) -> None:
        if not self_id:
            raise ValueError("self_id must be non-empty")
        self.self_id = self_id
        self.verifier: ContractVerifier = (
            verifier if verifier is not None else reject_all_verifier
        )
        self.audit = audit if audit is not None else AuditLog()
        self.max_age_s = float(max_age_s)
        self._peers: dict[str, PeerIdentity] = {}
        self._contracts: dict[str, PeerContract] = {}

    # -- membership ----------------------------------------------------------
    @property
    def peer_ids(self) -> tuple[str, ...]:
        return tuple(sorted(self._peers))

    def join(
        self,
        peer_id: str,
        public_key: str,
        contract: PeerContract,
    ) -> PeerIdentity:
        """Admit a peer, or refuse with a reason."""
        if peer_id == self.self_id:
            raise MeshRejected("a mesh cannot join itself")
        if not peer_id or not public_key:
            raise MeshRejected("peer_id and public_key are required")

        # S5 — an id belongs to the key that first claimed it.
        existing = self._peers.get(peer_id)
        if existing is not None and not hmac.compare_digest(
            existing.public_key, public_key
        ):
            self._audit("mesh.impersonation_refused", peer_id, "key mismatch")
            raise MeshRejected(
                f"peer id {peer_id} is already bound to a different key (S5)"
            )

        # S11 — no contract, no join.
        if contract.agent_id != peer_id:
            self._audit("mesh.join_refused", peer_id, "contract id mismatch")
            raise MeshRejected(
                f"contract agent_id {contract.agent_id!r} != peer_id {peer_id!r} (S11)"
            )
        if not self.verifier(contract):
            self._audit("mesh.join_refused", peer_id, "contract verification failed")
            raise MeshRejected(f"contract verification failed for {peer_id} (S11)")

        identity = PeerIdentity(
            peer_id=peer_id, public_key=public_key, joined_ts=time.time()
        )
        self._peers[peer_id] = identity
        self._contracts[peer_id] = contract
        self._audit("mesh.join", peer_id, "admitted")
        return identity

    def leave(self, peer_id: str) -> None:
        self._peers.pop(peer_id, None)
        self._contracts.pop(peer_id, None)
        self._audit("mesh.leave", peer_id, "removed")

    def contract_of(self, peer_id: str) -> PeerContract | None:
        return self._contracts.get(peer_id)

    # -- messaging -----------------------------------------------------------
    def sign(self, message: MeshMessage, secret: str) -> MeshMessage:
        """Attach an HMAC over (origin, topic, root, ts)."""
        message.signature = self._mac(message, secret)
        return message

    def _mac(self, message: MeshMessage, secret: str) -> str:
        blob = "|".join(
            (message.origin_id, message.topic, message.merkle_root, f"{message.ts:.6f}")
        )
        return hmac.new(
            secret.encode("utf-8"), blob.encode("utf-8"), hashlib.sha256
        ).hexdigest()

    def accept(self, message: MeshMessage, secret: str | None = None) -> MeshMessage:
        """Admit an inbound message, or refuse with a reason."""
        if message.origin_id == self.self_id:
            raise MeshRejected("refusing a message from self")
        if message.origin_id not in self._peers:
            self._audit("mesh.msg_refused", message.origin_id, "unknown peer")
            raise MeshRejected(f"unknown peer: {message.origin_id} (S11)")

        age = time.time() - message.ts
        if age > self.max_age_s:
            self._audit("mesh.msg_refused", message.origin_id, "stale")
            raise MeshRejected(f"message too old ({age:.0f}s > {self.max_age_s:.0f}s)")

        # S9 — the payload must hash to the claimed root.
        if not verify_payload(list(message.payload), message.merkle_root):
            self._audit("mesh.msg_refused", message.origin_id, "merkle mismatch")
            raise MeshRejected("merkle verification failed (S9)")

        if secret is not None:
            expected = self._mac(message, secret)
            if not hmac.compare_digest(expected, message.signature):
                self._audit("mesh.msg_refused", message.origin_id, "bad signature")
                raise MeshRejected("signature verification failed (S5)")

        self._audit("mesh.msg_accepted", message.origin_id, "accepted")
        return message

    def _audit(self, action: str, peer_id: str, detail: str) -> None:
        self.audit.append_action(
            ddr=DDR_ID,
            file=_FILE,
            action=action,
            gate_status=detail,
            peer_id=peer_id,
            self_id=self.self_id,
        )
