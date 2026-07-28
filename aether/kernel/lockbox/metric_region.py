"""DDR-795 — the Python side of the kernel metric-region wire.

Parses the sealed objective-function page the kernel maps read-only into every
user address space, and checks it against what the F#68 :class:`MetricLockbox`
believes.

Why this exists: F#68 keeps the metric content and its hash chain in one file,
so anything able to write that file can rewrite the entries *and* recompute the
chain over them. The chain proves internal consistency; it cannot prove the
history was not replaced wholesale. The kernel holds the root somewhere ring 3
cannot write, and :func:`verify_against` is where the two are compared — a
rewritten store cannot manufacture agreement.

**Scope, stated plainly.** The Python AETHER layer runs on the *host*, not in
PradyOS ring 3, so nothing here reads the live page at runtime; the path from
ring 3 to the host is the AETHER daemon and that is a separate slice. What this
module does is own the *layout*, so the C struct and the Python parser cannot
drift apart silently — the test builds a region with this module and asserts the
offsets match `kernel/aether/metric_page.h` field for field.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Any

__all__ = [
    "MetricRegion",
    "MetricRegionError",
    "parse_region",
    "build_region",
    "verify_against",
    "METRIC_MAGIC",
    "METRIC_VERSION",
    "METRIC_ROOT_LEN",
    "METRIC_PAGE_SIZE",
    "METRIC_USER_VA",
    "METRIC_FLAG_SEALED",
]

DDR_ID = "DDR-795"

METRIC_MAGIC = 0x4D455452          # "METR"
METRIC_VERSION = 1
METRIC_ROOT_LEN = 32
METRIC_PAGE_SIZE = 4096
METRIC_FLAG_SEALED = 1 << 0

#: Must match METRIC_USER_VA in kernel/aether/metric_page.h.
METRIC_USER_VA = 0x00007FFFFFEFF000

#: magic, version, generation, sealed_ts, root[32], entries, flags.
#: '<' is explicit: the layout is little-endian on the wire regardless of what
#: the host happens to be, because the producer is the kernel and the consumer
#: might not be the same machine.
_HEADER = struct.Struct("<II Q Q 32s I I")
assert _HEADER.size == 64, "metric region header must be 64 bytes"


class MetricRegionError(ValueError):
    """The region is malformed or does not match the lockbox."""


@dataclass(slots=True)
class MetricRegion:
    magic: int
    version: int
    generation: int
    sealed_ts: int
    root: bytes
    entries: int
    flags: int

    @property
    def sealed(self) -> bool:
        return bool(self.flags & METRIC_FLAG_SEALED)

    @property
    def root_hex(self) -> str:
        return self.root.hex()


def parse_region(data: bytes) -> MetricRegion:
    """Parse a raw page. Refuses anything it does not fully understand."""
    if len(data) < _HEADER.size:
        raise MetricRegionError(
            f"region is {len(data)} bytes; need at least {_HEADER.size}"
        )
    magic, version, generation, sealed_ts, root, entries, flags = _HEADER.unpack(
        data[: _HEADER.size]
    )
    if magic != METRIC_MAGIC:
        raise MetricRegionError(
            f"bad magic {magic:#010x}; expected {METRIC_MAGIC:#010x} — the "
            f"region is not mapped, or is not a metric region"
        )
    if version != METRIC_VERSION:
        # Refused, not best-effort parsed. A layout change that is silently
        # reinterpreted is exactly the failure this mechanism exists to prevent.
        raise MetricRegionError(
            f"region version {version} != {METRIC_VERSION}; refusing to guess "
            f"at a layout this build does not know"
        )
    return MetricRegion(magic, version, generation, sealed_ts, root, entries,
                        flags)


def build_region(
    root: bytes, entries: int, generation: int = 1, sealed_ts: int = 0,
    sealed: bool = True,
) -> bytes:
    """Produce a page in the kernel's layout. For tests and for the daemon."""
    if len(root) != METRIC_ROOT_LEN:
        raise MetricRegionError(
            f"root is {len(root)} bytes; expected {METRIC_ROOT_LEN}"
        )
    header = _HEADER.pack(
        METRIC_MAGIC, METRIC_VERSION, generation, sealed_ts, root, entries,
        METRIC_FLAG_SEALED if sealed else 0,
    )
    return header + b"\x00" * (METRIC_PAGE_SIZE - len(header))


def verify_against(region: MetricRegion, lockbox: Any) -> bool:
    """Does the kernel-sealed root match what the F#68 lockbox computes?

    Raises rather than returning False on a mismatch: a caller that ignores a
    boolean here is a caller running on an objective function nobody sealed, and
    that must not be a quiet outcome.
    """
    if not region.sealed:
        raise MetricRegionError(
            "region carries no sealed root; the kernel has not sealed an "
            "objective function yet"
        )
    computed = _lockbox_root(lockbox)
    if computed != region.root:
        raise MetricRegionError(
            f"objective-function root mismatch: kernel sealed "
            f"{region.root.hex()}, lockbox computes {computed.hex()} — the "
            f"metric store is not the one the kernel sealed"
        )
    active = len(lockbox.active())
    if active != region.entries:
        raise MetricRegionError(
            f"entry count mismatch: kernel sealed {region.entries}, lockbox "
            f"has {active} active"
        )
    return True


def _lockbox_root(lockbox: Any) -> bytes:
    """The F#68 chain head, as raw bytes.

    The lockbox's own head hash IS the root — it already chains every entry over
    its predecessor, so re-hashing here would invent a second definition of
    "the root" and guarantee the two drift.
    """
    import hashlib

    entries = lockbox.entries
    if not entries:
        return hashlib.sha256(b"").digest()
    return bytes.fromhex(entries[-1].entry_hash)
