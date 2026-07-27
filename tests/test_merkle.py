"""B-03 discriminating gate — Merkle integrity verification."""

from __future__ import annotations

import pytest

from kernel.integrity.merkle import (
    leaf_hash,
    merkle_proof,
    merkle_root,
    verify_payload,
    verify_proof,
)


def test_root_is_stable_and_order_sensitive() -> None:
    a = merkle_root([b"one", b"two", b"three"])
    b = merkle_root([b"one", b"two", b"three"])
    c = merkle_root([b"two", b"one", b"three"])
    assert a == b
    assert a != c            # reordering must change the root


def test_single_leaf_root_is_leaf_hash() -> None:
    assert merkle_root([b"solo"]) == leaf_hash(b"solo")


def test_verify_payload_accepts_and_rejects() -> None:
    leaves = [b"alpha", b"beta", b"gamma", b"delta"]
    root = merkle_root(leaves)
    assert verify_payload(leaves, root) is True
    # A tampered leaf must fail — this is the S9 gate for gossip packets.
    assert verify_payload([b"alpha", b"beta", b"gamma", b"TAMPERED"], root) is False
    assert verify_payload(leaves, "") is False
    assert verify_payload(leaves, "deadbeef") is False


@pytest.mark.parametrize("count", [1, 2, 3, 4, 5, 8, 9])
def test_inclusion_proof_roundtrip(count: int) -> None:
    leaves = [f"leaf-{i}".encode() for i in range(count)]
    root = merkle_root(leaves)
    for idx in range(count):
        proof = merkle_proof(leaves, idx)
        assert verify_proof(leaves[idx], proof, root) is True
        # A proof must not validate a different leaf.
        assert verify_proof(b"not-in-tree", proof, root) is False


def test_odd_node_promotion_is_not_duplication() -> None:
    """Duplicating the last node would make these two trees collide.

    The classic CVE-2012-2459 shape: with duplication, [a,b,c] and [a,b,c,c]
    hash to the same root. Promotion keeps them distinct.
    """
    three = merkle_root([b"a", b"b", b"c"])
    four = merkle_root([b"a", b"b", b"c", b"c"])
    assert three != four


def test_empty_tree_rejected() -> None:
    with pytest.raises(ValueError, match="at least one leaf"):
        merkle_root([])


def test_bad_index_rejected() -> None:
    with pytest.raises(IndexError):
        merkle_proof([b"a", b"b"], 5)


def test_str_and_bytes_leaves_agree() -> None:
    assert leaf_hash("text") == leaf_hash(b"text")
    with pytest.raises(TypeError):
        leaf_hash(123)  # type: ignore[arg-type]
