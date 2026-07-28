"""DDR-795 gate — the Python side of the kernel metric-region wire."""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import CAP_SOVEREIGN, Lockbox
from aether.kernel.lockbox.metric_lockbox import MetricLockbox
from aether.kernel.lockbox.metric_region import (
    METRIC_MAGIC,
    METRIC_PAGE_SIZE,
    METRIC_ROOT_LEN,
    METRIC_USER_VA,
    METRIC_VERSION,
    MetricRegion,
    MetricRegionError,
    build_region,
    parse_region,
    verify_against,
)

_HEADER_C = Path(__file__).resolve().parents[2] / "kernel/aether/metric_page.h"


def _sealed_lockbox(tmp_path: Path) -> MetricLockbox:
    audit = AuditLog(tmp_path / "audit_log.jsonl")
    lb = Lockbox(registry_path=tmp_path / "tokens.jsonl", audit=audit)
    root = lb.register("ROOT", "boot.py", "CAP_WORLD_MODEL_WRITE")
    object.__setattr__(root, "capability_mask", (CAP_SOVEREIGN,))
    lb._by_ddr["ROOT"] = root
    lb.register("SOVEREIGN", "kernel.py", CAP_SOVEREIGN, approved_by=root)
    ml = MetricLockbox(lockbox=lb, store_path=tmp_path / "metrics.jsonl",
                       audit=audit)
    ml.define("SOVEREIGN", "task_success", "fraction of gates that pass")
    return ml


# -- the layout must not drift from the C struct ------------------------------
def test_constants_match_the_kernel_header() -> None:
    """THE discriminator for this module.

    Two definitions of one layout drift, and the drift is silent until something
    reads a field from the wrong offset. The C header is the producer, so it is
    the authority — these constants are checked against it rather than against
    a comment.
    """
    src = _HEADER_C.read_text(encoding="utf-8")

    assert re.search(r"#define\s+METRIC_MAGIC\s+0x4D455452u", src), \
        "kernel magic changed"
    assert re.search(rf"#define\s+METRIC_VERSION\s+{METRIC_VERSION}u", src), \
        "kernel version changed without updating the Python side"
    assert re.search(rf"#define\s+METRIC_ROOT_LEN\s+{METRIC_ROOT_LEN}u", src), \
        "root length changed"
    assert f"{METRIC_USER_VA:012X}" in src.upper().replace("0X", ""), \
        "METRIC_USER_VA differs between the kernel header and Python"


def test_header_is_64_bytes_and_page_is_4k() -> None:
    """The C struct is _Static_assert'd to 4096; the parser must agree, or a
    region built here would be rejected by the kernel and vice versa."""
    region = build_region(b"\xAB" * METRIC_ROOT_LEN, entries=1)
    assert len(region) == METRIC_PAGE_SIZE
    assert "_Static_assert(sizeof(metric_page_t) == 4096" in \
        _HEADER_C.read_text(encoding="utf-8")


def test_field_offsets_match_the_documented_layout() -> None:
    """Offsets are asserted individually — a struct that happens to total 4096
    can still have every field in the wrong place."""
    root = bytes(range(METRIC_ROOT_LEN))
    raw = build_region(root, entries=7, generation=3, sealed_ts=12345)
    assert int.from_bytes(raw[0:4], "little") == METRIC_MAGIC
    assert int.from_bytes(raw[4:8], "little") == METRIC_VERSION
    assert int.from_bytes(raw[8:16], "little") == 3           # generation
    assert int.from_bytes(raw[16:24], "little") == 12345      # sealed_ts
    assert raw[24:56] == root                                  # root
    assert int.from_bytes(raw[56:60], "little") == 7          # entries
    assert int.from_bytes(raw[60:64], "little") == 1          # flags: sealed


# -- parsing -------------------------------------------------------------------
def test_round_trip(tmp_path: Path) -> None:
    root = b"\x11" * METRIC_ROOT_LEN
    region = parse_region(build_region(root, entries=4, generation=9))
    assert region.magic == METRIC_MAGIC
    assert region.root == root
    assert region.entries == 4
    assert region.generation == 9
    assert region.sealed is True
    assert region.root_hex == root.hex()


def test_bad_magic_is_refused() -> None:
    """A zeroed page reads as magic=0. Treating that as 'no objective function'
    rather than 'the region is not mapped' conflates two different states."""
    with pytest.raises(MetricRegionError, match="bad magic"):
        parse_region(b"\x00" * METRIC_PAGE_SIZE)


def test_unknown_version_is_refused_not_guessed() -> None:
    """A layout change silently reinterpreted is precisely the failure this
    whole mechanism exists to prevent."""
    raw = bytearray(build_region(b"\x00" * METRIC_ROOT_LEN, entries=1))
    raw[4:8] = (METRIC_VERSION + 1).to_bytes(4, "little")
    with pytest.raises(MetricRegionError, match="refusing to guess"):
        parse_region(bytes(raw))


def test_truncated_region_is_refused() -> None:
    with pytest.raises(MetricRegionError, match="need at least"):
        parse_region(b"\x00" * 16)


def test_wrong_root_length_refused() -> None:
    with pytest.raises(MetricRegionError, match="expected 32"):
        build_region(b"\x01" * 8, entries=1)


# -- the actual check ----------------------------------------------------------
def test_matching_root_verifies(tmp_path: Path) -> None:
    ml = _sealed_lockbox(tmp_path)
    root = bytes.fromhex(ml.entries[-1].entry_hash)
    region = parse_region(build_region(root, entries=len(ml.active())))
    assert verify_against(region, ml) is True


def test_rewritten_store_cannot_manufacture_agreement(tmp_path: Path) -> None:
    """THE point of the whole wire.

    F#68's chain proves the store is internally consistent. It cannot prove the
    history was not replaced, because the content and the chain live in the same
    file. Here the store is legitimately superseded — a change the lockbox
    itself accepts and re-chains — and it STILL fails against the root the
    kernel sealed earlier. That is the guarantee ring 3 cannot forge.
    """
    ml = _sealed_lockbox(tmp_path)
    sealed_root = bytes.fromhex(ml.entries[-1].entry_hash)
    region = parse_region(build_region(sealed_root, entries=len(ml.active())))
    assert verify_against(region, ml) is True

    ml.supersede("SOVEREIGN", "task_success", "whatever I want it to be",
                 reason="quietly redefining success")
    assert ml.verify() is True                    # internally consistent...
    with pytest.raises(MetricRegionError, match="not the one the kernel sealed"):
        verify_against(region, ml)                # ...and still caught


def test_mismatch_raises_rather_than_returning_false(tmp_path: Path) -> None:
    """A caller that ignores a boolean here runs on an objective function nobody
    sealed. That must not be a quiet outcome."""
    ml = _sealed_lockbox(tmp_path)
    region = parse_region(build_region(b"\xFF" * METRIC_ROOT_LEN, entries=1))
    with pytest.raises(MetricRegionError, match="root mismatch"):
        verify_against(region, ml)


def test_entry_count_mismatch_is_caught(tmp_path: Path) -> None:
    ml = _sealed_lockbox(tmp_path)
    root = bytes.fromhex(ml.entries[-1].entry_hash)
    region = parse_region(build_region(root, entries=99))
    with pytest.raises(MetricRegionError, match="entry count mismatch"):
        verify_against(region, ml)


def test_unsealed_region_is_refused(tmp_path: Path) -> None:
    """An unsealed page means the kernel never sealed anything — verifying
    against it would report success on a guarantee that does not exist."""
    ml = _sealed_lockbox(tmp_path)
    root = bytes.fromhex(ml.entries[-1].entry_hash)
    region = parse_region(build_region(root, entries=1, sealed=False))
    with pytest.raises(MetricRegionError, match="no sealed root"):
        verify_against(region, ml)
