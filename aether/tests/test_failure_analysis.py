"""B-16 discriminating gate — introspective failure analysis."""

from __future__ import annotations

from pathlib import Path

from aether.agents.introspection.failure_analysis import (
    FailureAnalyser,
    FailureRecord,
)
from aether.kernel.audit.audit_log import AuditLog, AuditRecord


def _analyser(tmp_path: Path) -> FailureAnalyser:
    return FailureAnalyser(audit=AuditLog(tmp_path / "audit_log.jsonl"))


def test_reads_only_failures_from_the_audit_log(tmp_path: Path) -> None:
    log = AuditLog(tmp_path / "audit_log.jsonl")
    log.append(AuditRecord(ddr="B-10", file="f", action="write", gate_status="executed"))
    log.append(AuditRecord(ddr="B-10", file="f", action="write", gate_status="denied"))
    fa = _analyser(tmp_path)
    failures = fa.read_failures()
    assert len(failures) == 1
    assert failures[0].status == "denied"


def test_clusters_on_shape_not_on_message(tmp_path: Path) -> None:
    """The discriminator.

    Real failure details carry paths, ids and numbers. Clustering on the raw
    message yields one cluster per occurrence, so a system failing the same way
    repeatedly reports 'no recurring failures'. These three are one shape.
    """
    fa = _analyser(tmp_path)
    records = [
        FailureRecord("B-10", "write", "denied", "path escapes root: /tmp/a/17.txt", 1.0),
        FailureRecord("B-10", "write", "denied", "path escapes root: /var/b/238.txt", 2.0),
        FailureRecord("B-10", "write", "denied", "path escapes root: /home/c/9.txt", 3.0),
    ]
    clusters = fa.cluster(records)
    assert len(clusters) == 1, [c.signature for c in clusters]
    assert clusters[0].count == 3
    assert clusters[0].recurring is True


def test_distinct_shapes_stay_separate(tmp_path: Path) -> None:
    fa = _analyser(tmp_path)
    records = [
        FailureRecord("B-10", "write", "denied", "path escapes root: /a/1.txt", 1.0),
        FailureRecord("B-10", "write", "denied", "path escapes root: /a/2.txt", 2.0),
        FailureRecord("B-04", "scan", "blocked", "rule PI-001 fired", 3.0),
    ]
    clusters = fa.cluster(records)
    assert len(clusters) == 2
    assert clusters[0].count == 2               # most frequent first


def test_recurring_needs_three_occurrences(tmp_path: Path) -> None:
    fa = _analyser(tmp_path)
    twice = [
        FailureRecord("B-09", "infer", "error", "timeout after 30s", 1.0),
        FailureRecord("B-09", "infer", "error", "timeout after 45s", 2.0),
    ]
    assert fa.recurring(twice) == []
    thrice = twice + [FailureRecord("B-09", "infer", "error", "timeout after 60s", 3.0)]
    assert len(fa.recurring(thrice)) == 1


def test_normalise_strips_volatile_parts() -> None:
    n = FailureAnalyser.normalise
    assert n("failed at 0xDEADBEEF") == n("failed at 0xCAFEBABE")
    assert n("wrote /a/b/c.txt") == n("wrote /x/y/z.txt")
    assert n("retry 17") == n("retry 4")
    assert n("id 123e4567-e89b-12d3-a456-426614174000") == n(
        "id 00000000-0000-0000-0000-000000000000"
    )


def test_end_to_end_from_real_audit_records(tmp_path: Path) -> None:
    """Feed it genuine audit output, not synthetic records."""
    log = AuditLog(tmp_path / "audit_log.jsonl")
    for i in range(4):
        log.append(
            AuditRecord(
                ddr="B-10", file="f", action="computer_use.write_file",
                gate_status="approval_denied",
                extra={"plan": f"CREATE report{i}.txt ({i} bytes)"},
            )
        )
    fa = _analyser(tmp_path)
    recurring = fa.recurring()
    assert len(recurring) == 1
    assert recurring[0].count == 4
    assert recurring[0].ddr == "B-10"


def test_by_ddr_counts(tmp_path: Path) -> None:
    fa = _analyser(tmp_path)
    records = [
        FailureRecord("B-04", "scan", "blocked", "x", 1.0),
        FailureRecord("B-04", "scan", "blocked", "y", 2.0),
        FailureRecord("B-10", "write", "denied", "z", 3.0),
    ]
    assert fa.by_ddr(records) == {"B-04": 2, "B-10": 1}


def test_report_shape(tmp_path: Path) -> None:
    fa = _analyser(tmp_path)
    records = [FailureRecord("B-09", "infer", "error", "boom", float(i)) for i in range(3)]
    report = fa.report(records)
    assert report["total_failures"] == 3
    assert report["cluster_count"] == 1
    assert report["recurring"][0]["ddr"] == "B-09"


def test_empty_log_is_not_an_error(tmp_path: Path) -> None:
    fa = _analyser(tmp_path)
    assert fa.cluster() == []
    assert fa.report()["total_failures"] == 0
