"""JSONL trajectory log — append-only run record (3D #51, DDR-849).

One JSON object per line, appended, never rewritten. The format is chosen for a
specific failure: a partially-written run must still be readable. A single JSON
array truncated mid-write is not valid JSON at all and takes the whole run with
it; JSONL truncated mid-write loses exactly the last line.

**Append-only is enforced, not merely intended.** There is no update, no delete,
no rewrite. A trajectory an agent can edit is a trajectory that says whatever
the agent's last revision said, which is worth less than no record — a record
that can be quietly corrected is one an operator will trust.

Secrets are redacted on the way IN, not on the way out. Redacting at read time
means the secret is on disk, and every future reader is one code path away from
printing it.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from typing import Any, Iterator, TextIO

__all__ = ["TrajectoryWriter", "redact", "REDACTED", "SECRET_KEYS", "MAX_FIELD_CHARS"]

REDACTED = "[REDACTED]"

# Key names whose VALUES are never written, matched case-insensitively as
# substrings so `openai_api_key` and `authToken` are both caught. Substring
# matching is deliberately over-broad: a redaction that fires when it did not
# need to costs a field in a log, and one that fails to fire costs a credential.
SECRET_KEYS = (
    "key", "token", "secret", "password", "passwd", "credential",
    "authorization", "cookie", "session", "signature", "privkey",
)

# Values that look like a secret regardless of the key they arrived under.
_SECRET_VALUE_RES = (
    re.compile(r"\bghp_[A-Za-z0-9]{16,}\b"),          # GitHub PAT
    re.compile(r"\bgithub_pat_[A-Za-z0-9_]{20,}\b"),
    re.compile(r"\bsk-[A-Za-z0-9_\-]{16,}\b"),        # OpenAI-style
    re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----"),
)

# A single oversized field can make a whole run unreadable in ordinary tools.
MAX_FIELD_CHARS = 8192

_MAX_DEPTH = 12


def _looks_secret(key: str) -> bool:
    low = key.lower()
    return any(marker in low for marker in SECRET_KEYS)


def _scrub_value(value: str) -> str:
    for pattern in _SECRET_VALUE_RES:
        value = pattern.sub(REDACTED, value)
    if len(value) > MAX_FIELD_CHARS:
        value = value[:MAX_FIELD_CHARS] + f"...[truncated {len(value)} chars]"
    return value


def redact(obj: Any, _depth: int = 0) -> Any:
    """Recursively redact secrets. Applied before anything reaches disk.

    Depth-bounded: a self-referential structure would otherwise recurse until
    the process dies, and a logger that can kill the run it is logging is worse
    than no logger.
    """
    if _depth > _MAX_DEPTH:
        return "[TRUNCATED: max depth]"
    if isinstance(obj, dict):
        return {
            k: (REDACTED if _looks_secret(str(k)) else redact(v, _depth + 1))
            for k, v in obj.items()
        }
    if isinstance(obj, (list, tuple)):
        return [redact(v, _depth + 1) for v in obj]
    if isinstance(obj, str):
        return _scrub_value(obj)
    if isinstance(obj, (int, float, bool)) or obj is None:
        return obj
    return _scrub_value(str(obj))


@dataclass
class TrajectoryWriter:
    """Append-only JSONL writer. No update path exists, by construction."""

    stream: TextIO
    run_id: str
    _seq: int = 0

    def __post_init__(self) -> None:
        if not self.run_id.strip():
            raise ValueError("run_id must be set — an unattributed trajectory "
                             "cannot be matched to the run that produced it")

    def append(self, event: str, **fields: Any) -> dict[str, Any]:
        """Write one event. Returns the record as written, post-redaction.

        Returning the redacted record rather than the caller's input means a
        caller that echoes the return value cannot re-leak what was just
        scrubbed.
        """
        if not event.strip():
            raise ValueError("event name is required")
        self._seq += 1
        record = {
            "run_id": self.run_id,
            "seq": self._seq,
            "event": event,
            **redact(fields),
        }
        # separators avoids incidental whitespace drift between writers;
        # ensure_ascii keeps every line byte-safe on a serial console.
        line = json.dumps(record, separators=(",", ":"), ensure_ascii=True, default=str)
        if "\n" in line:  # pragma: no cover - json.dumps escapes these
            raise AssertionError("record produced an embedded newline")
        self.stream.write(line + "\n")
        self.stream.flush()
        return record


def read_trajectory(stream: TextIO, strict: bool = False) -> Iterator[dict[str, Any]]:
    """Read a JSONL trajectory, tolerating a truncated final line.

    The tolerance is the reason for the format: a run killed mid-write loses its
    last line and nothing else. `strict=True` raises instead, for the callers
    that would rather know.
    """
    for lineno, raw in enumerate(stream, 1):
        raw = raw.strip()
        if not raw:
            continue
        try:
            yield json.loads(raw)
        except json.JSONDecodeError:
            if strict:
                raise
            # A partial line can only ever be the last one; anything else is
            # corruption in the middle and must not pass silently.
            remainder = stream.read().strip()
            if remainder:
                raise ValueError(
                    f"malformed JSONL at line {lineno} with {len(remainder)} "
                    "bytes following — this is mid-file corruption, not a "
                    "truncated tail, and must not be tolerated"
                )
            return
