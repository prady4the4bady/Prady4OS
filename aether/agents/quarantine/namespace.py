"""B-05 — quarantine namespace.

A sandbox for untrusted content: downloaded pages, peer packets, tool output,
anything a red-team agent is poking at. Two jobs:

1. **Containment.** Every path is resolved inside a namespace root. Traversal
   (``../``), absolute paths, and symlinks pointing outside are refused —
   resolved *after* following links, because a symlink created inside the root
   is the obvious way around a naive prefix check.

2. **Taint.** Content that enters is tainted, and taint is sticky: anything
   derived from tainted content is tainted too. Tainted text cannot be released
   for use in a prompt except through :meth:`QuarantineNamespace.release`, which
   routes it through the B-04 firewall. That is how **S7** stays true for data
   that arrives via the filesystem rather than as a direct string.

**S8** is served by ``dry_run``: a namespace opened dry performs every check and
reports what *would* change, but touches nothing on disk.
"""

from __future__ import annotations

import shutil
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from aether.agents.firewall.prompt_injection import (
    PromptFirewall,
    PromptInjectionBlocked,
)
from aether.kernel.audit.audit_log import AuditLog

__all__ = [
    "QuarantineViolation",
    "TaintedContent",
    "QuarantineNamespace",
]

DDR_ID = "B-05"
_FILE = "aether/agents/quarantine/namespace.py"


class QuarantineViolation(PermissionError):
    """Raised when an operation would escape the namespace."""


@dataclass(slots=True)
class TaintedContent:
    """Untrusted bytes/text plus its provenance.

    Taint is sticky: :meth:`derive` returns tainted content, never clean.
    """

    data: str
    origin: str
    tainted: bool = True
    lineage: tuple[str, ...] = field(default_factory=tuple)

    def derive(self, new_data: str, step: str) -> TaintedContent:
        return TaintedContent(
            data=new_data,
            origin=self.origin,
            tainted=True,
            lineage=self.lineage + (step,),
        )


class QuarantineNamespace:
    """An isolated filesystem + taint scope for untrusted work."""

    def __init__(
        self,
        root: str | Path | None = None,
        dry_run: bool = False,
        firewall: PromptFirewall | None = None,
        audit: AuditLog | None = None,
    ) -> None:
        if root is None:
            self._owns_root = True
            root = Path(tempfile.mkdtemp(prefix="aether-quarantine-"))
        else:
            self._owns_root = False
            root = Path(root)
            root.mkdir(parents=True, exist_ok=True)
        # resolve() so the stored root is already symlink-free; comparisons
        # against it are then meaningful.
        self.root: Path = root.resolve()
        self.dry_run = bool(dry_run)
        self.firewall = firewall if firewall is not None else PromptFirewall()
        self.audit = audit if audit is not None else AuditLog()
        self._planned: list[dict[str, Any]] = []

    # -- containment ---------------------------------------------------------
    def resolve(self, relative: str | Path) -> Path:
        """Resolve ``relative`` inside the namespace, or raise.

        Refuses absolute paths, parent traversal, and symlinks whose target
        lands outside the root. The check is done on the fully-resolved path,
        because a symlink planted inside the root defeats a string-prefix test.
        """
        rel = Path(relative)
        raw = str(relative)
        # Path.is_absolute() is platform-dependent: on Windows "/etc/passwd" is
        # drive-RELATIVE and reports False, so the POSIX form would slip past
        # this check on a dev box while being absolute on the Linux target.
        # Test the textual forms too so the rule is identical on both.
        drive_qualified = len(raw) > 1 and raw[1] == ":"
        if rel.is_absolute() or raw.startswith(("/", "\\")) or drive_qualified:
            raise QuarantineViolation(f"absolute path not allowed: {relative}")
        if any(part == ".." for part in rel.parts):
            raise QuarantineViolation(f"parent traversal not allowed: {relative}")

        candidate = (self.root / rel).resolve()
        if candidate != self.root and self.root not in candidate.parents:
            raise QuarantineViolation(
                f"path escapes quarantine root: {relative} -> {candidate}"
            )
        return candidate

    def write(self, relative: str | Path, data: str) -> Path:
        """Write inside the namespace. Under ``dry_run`` nothing is touched."""
        target = self.resolve(relative)
        if self.dry_run:
            self._planned.append({"action": "write", "path": str(target), "bytes": len(data)})
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="quarantine.dry_write",
                gate_status="planned", path=str(target),
            )
            return target
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(data, encoding="utf-8")
        self.audit.append_action(
            ddr=DDR_ID, file=str(target), action="quarantine.write",
            gate_status="written",
        )
        return target

    def read(self, relative: str | Path) -> TaintedContent:
        """Read from the namespace. The result is tainted by construction."""
        target = self.resolve(relative)
        if not target.is_file():
            raise FileNotFoundError(f"no such file in quarantine: {relative}")
        return TaintedContent(
            data=target.read_text(encoding="utf-8"),
            origin=f"quarantine:{target.relative_to(self.root)}",
        )

    # -- taint ---------------------------------------------------------------
    def ingest(self, data: str, origin: str) -> TaintedContent:
        """Bring untrusted data into the namespace as tainted content."""
        return TaintedContent(data=data, origin=origin)

    def release(self, content: TaintedContent) -> str:
        """Release tainted content for prompt use, via the B-04 firewall.

        This is the only sanctioned exit. A BLOCK verdict raises rather than
        returning degraded text, so a caller cannot accidentally use it.
        """
        if not isinstance(content, TaintedContent):
            raise TypeError(
                f"release expects TaintedContent, got {type(content).__name__}"
            )
        try:
            safe = self.firewall.guard(content.data, source=content.origin)
        except PromptInjectionBlocked:
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="quarantine.release_blocked",
                gate_status="blocked", origin=content.origin,
            )
            raise
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="quarantine.release",
            gate_status="released", origin=content.origin,
        )
        return safe

    # -- dry-run bookkeeping (S8) -------------------------------------------
    @property
    def planned_changes(self) -> tuple[dict[str, Any], ...]:
        """What a dry run *would* have done."""
        return tuple(self._planned)

    # -- lifecycle -----------------------------------------------------------
    def destroy(self) -> None:
        """Remove the namespace tree if this object created it."""
        if self.dry_run:
            self._planned.append({"action": "destroy", "path": str(self.root)})
            return
        if self._owns_root and self.root.is_dir():
            shutil.rmtree(self.root, ignore_errors=True)

    def __enter__(self) -> QuarantineNamespace:
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.destroy()
