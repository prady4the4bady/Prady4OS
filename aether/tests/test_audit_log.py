"""B-02 discriminating gate — append-only audit log."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from aether.kernel.audit.audit_log import AuditLog, AuditRecord, sha256_of_file


def test_append_and_read(tmp_path: Path) -> None:
    log = AuditLog(tmp_path / "audit_log.jsonl")
    log.append(
        AuditRecord(
            ddr="B-02", file="x.py", action="create", gate_status="pass"
        )
    )
    log.append(
        AuditRecord(
            ddr="B-02", file="y.py", action="update", gate_status="pass"
        )
    )
    records = log.read_all()
    assert len(records) == 2
    assert records[0]["ddr"] == "B-02"
    assert records[1]["action"] == "update"
    # Mandated field set is present on every record.
    for rec in records:
        for key in ("ts", "ddr", "file", "action", "gate_status", "sha256_of_file"):
            assert key in rec


def test_append_only_no_mutation_api(tmp_path: Path) -> None:
    """The log must expose no update/delete path — that is what makes S4/S5 hold."""
    log = AuditLog(tmp_path / "audit_log.jsonl")
    for forbidden in ("update", "delete", "truncate", "rewrite", "remove"):
        assert not hasattr(log, forbidden)


def test_appending_never_rewrites_earlier_lines(tmp_path: Path) -> None:
    path = tmp_path / "audit_log.jsonl"
    log = AuditLog(path)
    log.append(AuditRecord(ddr="B-02", file="a", action="one", gate_status="pass"))
    first_line = path.read_text(encoding="utf-8").splitlines()[0]
    log.append(AuditRecord(ddr="B-02", file="b", action="two", gate_status="pass"))
    lines = path.read_text(encoding="utf-8").splitlines()
    assert len(lines) == 2
    assert lines[0] == first_line          # earlier record byte-identical


def test_sha256_of_file(tmp_path: Path) -> None:
    f = tmp_path / "payload.txt"
    f.write_text("hello", encoding="utf-8")
    digest = sha256_of_file(f)
    assert digest == (
        "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
    )
    assert sha256_of_file(tmp_path / "missing.txt") == ""


def test_append_action_hashes_file(tmp_path: Path) -> None:
    f = tmp_path / "tracked.py"
    f.write_text("x = 1\n", encoding="utf-8")
    log = AuditLog(tmp_path / "audit_log.jsonl")
    rec = log.append_action(ddr="B-02", file=str(f), action="create", gate_status="pass")
    assert rec.sha256_of_file == sha256_of_file(f)


def test_rejects_non_record(tmp_path: Path) -> None:
    log = AuditLog(tmp_path / "audit_log.jsonl")
    with pytest.raises(TypeError):
        log.append({"ddr": "B-02"})  # type: ignore[arg-type]


def test_corrupt_record_is_reported(tmp_path: Path) -> None:
    path = tmp_path / "audit_log.jsonl"
    path.write_text("{not json}\n", encoding="utf-8")
    log = AuditLog(path)
    with pytest.raises(ValueError, match="corrupt audit record"):
        log.read_all()


def test_record_serialises_single_line() -> None:
    rec = AuditRecord(
        ddr="B-02", file="a\nb", action="create", gate_status="pass"
    )
    assert "\n" not in rec.to_json()
    assert json.loads(rec.to_json())["file"] == "a\nb"
