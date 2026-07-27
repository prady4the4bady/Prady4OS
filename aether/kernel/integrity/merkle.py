"""B-03 — Merkle tree integrity verification.

Backs invariant **S9 (never accept a knowledge packet without Merkle
verification)** for the federation layer (C-01), and the Merkle-chained ledger
half of **S5**.

Binary tree over SHA-256 leaves. An odd node count promotes the last node
unchanged rather than duplicating it — duplication is the classic CVE-2012-2459
second-preimage shape, where two distinct leaf lists yield the same root.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from typing import Sequence

__all__ = [
    "MerkleProofStep",
    "leaf_hash",
    "merkle_root",
    "merkle_proof",
    "verify_proof",
    "verify_payload",
]

_LEAF_PREFIX = b"\x00"
_NODE_PREFIX = b"\x01"


@dataclass(slots=True)
class MerkleProofStep:
    """One sibling on the path from a leaf to the root."""

    sibling: str
    is_right: bool


def _as_bytes(item: bytes | str) -> bytes:
    if isinstance(item, bytes):
        return item
    if isinstance(item, str):
        return item.encode("utf-8")
    raise TypeError(f"leaf must be bytes or str, got {type(item).__name__}")


def leaf_hash(item: bytes | str) -> str:
    """Domain-separated leaf hash."""
    return hashlib.sha256(_LEAF_PREFIX + _as_bytes(item)).hexdigest()


def _pair_hash(left: str, right: str) -> str:
    raw = _NODE_PREFIX + bytes.fromhex(left) + bytes.fromhex(right)
    return hashlib.sha256(raw).hexdigest()


def _levels(leaves: Sequence[bytes | str]) -> list[list[str]]:
    if not leaves:
        raise ValueError("merkle tree requires at least one leaf")
    level = [leaf_hash(item) for item in leaves]
    levels = [level]
    while len(level) > 1:
        nxt: list[str] = []
        for i in range(0, len(level) - 1, 2):
            nxt.append(_pair_hash(level[i], level[i + 1]))
        if len(level) % 2 == 1:
            nxt.append(level[-1])          # promote, never duplicate
        level = nxt
        levels.append(level)
    return levels


def merkle_root(leaves: Sequence[bytes | str]) -> str:
    """Hex root over ``leaves``."""
    return _levels(leaves)[-1][0]


def merkle_proof(leaves: Sequence[bytes | str], index: int) -> list[MerkleProofStep]:
    """Inclusion proof for ``leaves[index]``."""
    levels = _levels(leaves)
    if index < 0 or index >= len(levels[0]):
        raise IndexError(f"leaf index {index} out of range")
    proof: list[MerkleProofStep] = []
    idx = index
    for level in levels[:-1]:
        if idx % 2 == 0:
            sib = idx + 1
            if sib < len(level):
                proof.append(MerkleProofStep(sibling=level[sib], is_right=True))
            # else: promoted node — contributes no sibling, but the index must
            # still advance to its parent slot (idx // 2). Skipping that was a
            # real bug the odd-count gate caught.
        else:
            proof.append(MerkleProofStep(sibling=level[idx - 1], is_right=False))
        idx //= 2
    return proof


def verify_proof(item: bytes | str, proof: Sequence[MerkleProofStep], root: str) -> bool:
    """True when ``item`` + ``proof`` reconstruct ``root``."""
    cur = leaf_hash(item)
    for step in proof:
        cur = (
            _pair_hash(cur, step.sibling)
            if step.is_right
            else _pair_hash(step.sibling, cur)
        )
    return cur == root


def verify_payload(leaves: Sequence[bytes | str], claimed_root: str) -> bool:
    """True when ``leaves`` hash to ``claimed_root``.

    Used by the federation layer before accepting a gossip packet (S9).
    """
    if not claimed_root:
        return False
    try:
        return merkle_root(leaves) == claimed_root
    except (ValueError, TypeError):
        return False
