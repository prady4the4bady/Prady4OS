"""B-16 — introspective failure analysis.

Reads the audit log and turns raw failure records into clusters with a signature
and a frequency. D-01 (self-improvement proposals) consumes these directly, so
the clustering rule matters more than the plumbing.

The rule: cluster on **(ddr, action, error class)**, not on the error message.
Messages carry paths, ids and timestamps, so message-identity clustering yields
one cluster per occurrence and reports "no recurring failures" for a system that
is failing the same way a hundred times. Normalising the volatile parts out is
the whole job.
"""

from __future__ import annotations

import re
from collections import Counter
from dataclasses import dataclass, field
from typing import Any, Iterable

from aether.kernel.audit.audit_log import AuditLog

__all__ = [
    "FailureRecord",
    "FailureCluster",
    "FailureAnalyser",
    "FAILURE_STATUSES",
]

DDR_ID = "B-16"
_FILE = "aether/agents/introspection/failure_analysis.py"

#: gate_status values that count as a failure.
FAILURE_STATUSES: frozenset[str] = frozenset(
    {
        "error", "failed", "failure", "denied", "refused", "blocked",
        "rejected", "timeout", "approval_denied", "refused_unconfirmed",
    }
)

_HEX = re.compile(r"\b0x[0-9a-fA-F]+\b")
# No word boundaries: real details embed digits inside identifiers ("report0.txt",
# "timeout after 30s"). \b\d+\b misses those, so equivalent failures never
# collapse and the analyser reports "nothing recurring" for a system failing the
# same way repeatedly — the exact blindness this module exists to prevent.
_NUM = re.compile(r"\d+")
_PATH = re.compile(r"(?:[A-Za-z]:)?[\\/][\w.\\/-]+")
_UUID = re.compile(
    r"\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
    r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\b"
)


@dataclass(slots=True)
class FailureRecord:
    ddr: str
    action: str
    status: str
    detail: str = ""
    ts: float = 0.0


@dataclass(slots=True)
class FailureCluster:
    signature: str
    ddr: str
    action: str
    count: int = 0
    first_ts: float = 0.0
    last_ts: float = 0.0
    examples: list[str] = field(default_factory=list)

    @property
    def recurring(self) -> bool:
        return self.count >= 3


class FailureAnalyser:
    """Turns audit-log noise into a ranked list of recurring failure shapes."""

    def __init__(self, audit: AuditLog | None = None) -> None:
        self.audit = audit if audit is not None else AuditLog()

    # -- normalisation -------------------------------------------------------
    @staticmethod
    def normalise(detail: str) -> str:
        """Strip the volatile parts so equivalent failures collapse together."""
        out = _UUID.sub("<uuid>", detail)
        out = _PATH.sub("<path>", out)
        out = _HEX.sub("<hex>", out)
        out = _NUM.sub("<n>", out)
        return " ".join(out.split()).strip()

    @classmethod
    def signature(cls, record: FailureRecord) -> str:
        return f"{record.ddr}|{record.action}|{cls.normalise(record.detail)}"

    # -- ingestion -----------------------------------------------------------
    def read_failures(self) -> list[FailureRecord]:
        records: list[FailureRecord] = []
        for entry in self.audit.iter_records():
            status = str(entry.get("gate_status", "")).lower()
            if status not in FAILURE_STATUSES:
                continue
            extra = entry.get("extra", {}) or {}
            detail = str(
                extra.get("error")
                or extra.get("reason")
                or extra.get("plan")
                or extra.get("capability")
                or ""
            )
            records.append(
                FailureRecord(
                    ddr=str(entry.get("ddr", "")),
                    action=str(entry.get("action", "")),
                    status=status,
                    detail=detail,
                    ts=float(entry.get("ts", 0.0)),
                )
            )
        return records

    # -- clustering ----------------------------------------------------------
    def cluster(
        self, records: Iterable[FailureRecord] | None = None
    ) -> list[FailureCluster]:
        """Group failures by normalised signature, most frequent first."""
        source = list(records) if records is not None else self.read_failures()
        clusters: dict[str, FailureCluster] = {}
        for rec in source:
            sig = self.signature(rec)
            cluster = clusters.get(sig)
            if cluster is None:
                cluster = FailureCluster(
                    signature=sig, ddr=rec.ddr, action=rec.action,
                    first_ts=rec.ts, last_ts=rec.ts,
                )
                clusters[sig] = cluster
            cluster.count += 1
            cluster.first_ts = min(cluster.first_ts or rec.ts, rec.ts)
            cluster.last_ts = max(cluster.last_ts, rec.ts)
            if len(cluster.examples) < 3 and rec.detail:
                cluster.examples.append(rec.detail)
        return sorted(
            clusters.values(), key=lambda c: (-c.count, c.signature)
        )

    def recurring(
        self, records: Iterable[FailureRecord] | None = None
    ) -> list[FailureCluster]:
        """Clusters worth proposing a fix for (D-01's input)."""
        return [c for c in self.cluster(records) if c.recurring]

    def by_ddr(self, records: Iterable[FailureRecord] | None = None) -> dict[str, int]:
        source = list(records) if records is not None else self.read_failures()
        return dict(Counter(r.ddr for r in source))

    def report(self, records: Iterable[FailureRecord] | None = None) -> dict[str, Any]:
        clusters = self.cluster(records)
        return {
            "total_failures": sum(c.count for c in clusters),
            "cluster_count": len(clusters),
            "recurring": [
                {"signature": c.signature, "count": c.count, "ddr": c.ddr}
                for c in clusters
                if c.recurring
            ],
        }
