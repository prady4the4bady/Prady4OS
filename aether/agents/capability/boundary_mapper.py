"""C-05 — capability boundary mapper.

Continuously probes the edge between what the system can and cannot do, keeping
a living map per domain.

The distinction that makes the map useful: **boundary_confidence is confidence
in knowing where the boundary is, not in being capable.** A domain where every
probe succeeds and a domain where every probe fails are both *well mapped* — the
boundary is known. A domain split 50/50 is where the edge actually lies and is
the least understood. Scoring "confidence" as a success rate would rank a
totally-incapable domain as maximally uncertain, and send probing effort exactly
where it is least needed.

Probes are generated at the boundary by the B-06 red-team agent, so the map is
pushed outward rather than re-confirming easy wins.
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Callable

from aether.agents.red_team.red_team_agent import AttackFamily, RedTeamAgent
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "CapabilityDomain",
    "BoundaryMapper",
    "BoundaryError",
    "boundary_confidence",
]

DDR_ID = "C-05"
_FILE = "aether/agents/capability/boundary_mapper.py"
CAPABILITY = "CAP_CAPABILITY_PROBE"

DEFAULT_MAP = Path(__file__).resolve().parent / "boundary_map.jsonl"


class BoundaryError(RuntimeError):
    """Probe or map operation refused."""


def boundary_confidence(known: int, failed: int) -> float:
    """How well is this domain's boundary understood?

    1.0 when every probe agrees (all pass or all fail — the edge is clearly
    outside or inside), falling to 0.0 at an even split, where the boundary
    genuinely runs through the domain and more probing is warranted.
    """
    total = known + failed
    if total == 0:
        return 0.0
    balance = abs(known - failed) / total
    return round(balance, 4)


@dataclass(slots=True)
class CapabilityDomain:
    domain: str
    known_tasks: list[str] = field(default_factory=list)
    failure_tasks: list[str] = field(default_factory=list)
    updated_ts: float = field(default_factory=time.time)

    @property
    def boundary_confidence(self) -> float:
        return boundary_confidence(len(self.known_tasks), len(self.failure_tasks))

    @property
    def probes(self) -> int:
        return len(self.known_tasks) + len(self.failure_tasks)

    def to_json(self) -> str:
        payload = asdict(self)
        payload["boundary_confidence"] = self.boundary_confidence
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))


#: Executes a task; True means the system managed it.
ExecuteFn = Callable[[str, str], bool]


class BoundaryMapper:
    """Probes the capability edge and maintains the map."""

    def __init__(
        self,
        lockbox: Lockbox,
        execute_fn: ExecuteFn,
        red_team: RedTeamAgent | None = None,
        map_path: str | Path | None = None,
        audit: AuditLog | None = None,
    ) -> None:
        self.lockbox = lockbox
        self.execute_fn = execute_fn
        self.red_team = red_team if red_team is not None else RedTeamAgent()
        self.map_path = Path(map_path) if map_path is not None else DEFAULT_MAP
        self.map_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self._map: dict[str, CapabilityDomain] = {}
        self._replay()

    def _replay(self) -> None:
        if not self.map_path.is_file():
            return
        with self.map_path.open("r", encoding="utf-8") as fh:
            for raw in fh:
                stripped = raw.strip()
                if not stripped:
                    continue
                try:
                    rec = json.loads(stripped)
                except json.JSONDecodeError as exc:
                    raise BoundaryError(f"corrupt boundary map: {self.map_path}") from exc
                self._map[rec["domain"]] = CapabilityDomain(
                    domain=rec["domain"],
                    known_tasks=list(rec.get("known_tasks", [])),
                    failure_tasks=list(rec.get("failure_tasks", [])),
                    updated_ts=rec.get("updated_ts", 0.0),
                )

    def _persist(self, domain: CapabilityDomain) -> None:
        with self.map_path.open("a", encoding="utf-8") as fh:
            fh.write(domain.to_json() + "\n")

    # -- probing -------------------------------------------------------------
    def probe(self, domain: str, task: str) -> bool:
        """Attempt one task and record which side of the boundary it fell on."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not domain or not task:
            raise BoundaryError("probe needs a domain and a task")
        try:
            ok = bool(self.execute_fn(domain, task))
        except Exception as exc:
            # A raising task is a failure, not a crash of the mapper.
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="boundary.probe_error",
                gate_status="failed", domain=domain,
                error=f"{type(exc).__name__}: {exc}",
            )
            ok = False
        self.update_map(domain, task, ok)
        return ok

    def update_map(self, domain: str, task: str, result: bool) -> CapabilityDomain:
        entry = self._map.get(domain)
        if entry is None:
            entry = CapabilityDomain(domain=domain)
            self._map[domain] = entry
        bucket = entry.known_tasks if result else entry.failure_tasks
        other = entry.failure_tasks if result else entry.known_tasks
        # A task that flips sides moves, rather than appearing in both.
        if task in other:
            other.remove(task)
        if task not in bucket:
            bucket.append(task)
        entry.updated_ts = time.time()
        self._persist(entry)
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="boundary.update",
            gate_status="can" if result else "cannot",
            domain=domain, task=task,
            boundary_confidence=entry.boundary_confidence,
        )
        return entry

    def probe_adversarial(
        self, domain: str, family: AttackFamily, limit: int = 4
    ) -> list[tuple[str, bool]]:
        """Probe with red-team-generated cases at the edge, not easy wins."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        cases = self.red_team.generate(families=[family], max_mutations=1)[:limit]
        out: list[tuple[str, bool]] = []
        for case in cases:
            out.append((case.payload, self.probe(domain, case.payload)))
        return out

    # -- reading the map -----------------------------------------------------
    def export_map(self) -> dict[str, CapabilityDomain]:
        return dict(self._map)

    def get(self, domain: str) -> CapabilityDomain | None:
        return self._map.get(domain)

    def least_understood(self, limit: int = 3) -> list[CapabilityDomain]:
        """Domains where the boundary is least clear — probe these next."""
        probed = [d for d in self._map.values() if d.probes > 0]
        return sorted(probed, key=lambda d: (d.boundary_confidence, d.domain))[:limit]
