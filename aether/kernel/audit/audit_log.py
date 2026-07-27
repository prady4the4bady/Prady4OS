"""B-02 — append-only JSONL audit log.

Every mutating operation in the AETHER host layer writes one record here. The
file is append-only by construction: this module exposes no update or delete
path, which is what makes invariant **S5 (append-only audit)** enforceable and
**S4 (never forge or alter audit entries)** checkable.

Records carry the fields the ASI-bridge prompt mandates:
``{ts, ddr, file, action, gate_status, sha256_of_file}``.
"""

from __future__ import annotations

import hashlib
import json
import os
import threading
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Iterator

__all__ = [
    "AuditRecord",
    "AuditLog",
    "DEFAULT_LOG_PATH",
    "sha256_of_file",
    "append",
    "read_all",
]

DEFAULT_LOG_PATH = Path(__file__).resolve().parent / "audit_log.jsonl"

# One lock per process. Appends are also opened in "a" mode so a single write()
# of a line <= PIPE_BUF is atomic on POSIX; the lock covers the interpreter side.
_LOCK = threading.Lock()


@dataclass(slots=True)
class AuditRecord:
    """One append-only audit entry."""

    ddr: str
    file: str
    action: str
    gate_status: str
    sha256_of_file: str = ""
    ts: float = field(default_factory=time.time)
    extra: dict[str, Any] = field(default_factory=dict)

    def to_json(self) -> str:
        payload = asdict(self)
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def sha256_of_file(path: str | Path) -> str:
    """Hex SHA-256 of a file, or "" when it does not exist.

    A missing file is not an error: DDRs legitimately audit actions about paths
    that have not been written yet (a proposal, a rejected packet).
    """
    p = Path(path)
    if not p.is_file():
        return ""
    h = hashlib.sha256()
    with p.open("rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


class AuditLog:
    """Append-only JSONL log. No update or delete path exists, by design."""

    def __init__(self, path: str | Path | None = None) -> None:
        self.path = Path(path) if path is not None else DEFAULT_LOG_PATH
        self.path.parent.mkdir(parents=True, exist_ok=True)

    def append(self, record: AuditRecord) -> AuditRecord:
        if not isinstance(record, AuditRecord):
            raise TypeError(f"expected AuditRecord, got {type(record).__name__}")
        line = record.to_json()
        if "\n" in line:
            raise ValueError("audit record serialised to a multi-line string")
        with _LOCK:
            with self.path.open("a", encoding="utf-8") as fh:
                fh.write(line + "\n")
                fh.flush()
                os.fsync(fh.fileno())
        return record

    def append_action(
        self,
        ddr: str,
        file: str,
        action: str,
        gate_status: str = "n/a",
        **extra: Any,
    ) -> AuditRecord:
        """Convenience wrapper that hashes ``file`` for the caller."""
        return self.append(
            AuditRecord(
                ddr=ddr,
                file=file,
                action=action,
                gate_status=gate_status,
                sha256_of_file=sha256_of_file(file),
                extra=dict(extra),
            )
        )

    def read_all(self) -> list[dict[str, Any]]:
        return list(self.iter_records())

    def iter_records(self) -> Iterator[dict[str, Any]]:
        if not self.path.is_file():
            return
        with self.path.open("r", encoding="utf-8") as fh:
            for lineno, raw in enumerate(fh, start=1):
                stripped = raw.strip()
                if not stripped:
                    continue
                try:
                    yield json.loads(stripped)
                except json.JSONDecodeError as exc:
                    raise ValueError(
                        f"corrupt audit record at {self.path}:{lineno}"
                    ) from exc


_DEFAULT_LOG = AuditLog()


def append(record: AuditRecord) -> AuditRecord:
    """Append to the process-default audit log."""
    return _DEFAULT_LOG.append(record)


def read_all() -> list[dict[str, Any]]:
    """Read the process-default audit log."""
    return _DEFAULT_LOG.read_all()
