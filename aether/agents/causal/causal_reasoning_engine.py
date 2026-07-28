"""D-06 — causal reasoning engine.

Builds cause→effect structure from observation logs. The whole design turns on
one rule: **correlation never promotes itself to causation.**

Two variables that always move together are indistinguishable from two effects
of a hidden third by correlation alone, so a link becomes ``CAUSAL`` only with
evidence correlation cannot supply:

* **intervention** — observations where a variable was *set* rather than
  observed, and the candidate effect moved with it (do-calculus, in miniature);
  or
* **screening-off survival** — the pair stays correlated after conditioning on
  every other observed variable. If some ``Z`` drives the partial correlation to
  ~0, the pair is marked ``CONFOUNDED`` with ``Z`` named, not ``CAUSAL``.

Anything else stays ``CORRELATED``, which is an honest verdict rather than a
weak causal claim.

Everything here is arithmetic on logged observations — no model call — so it
satisfies **S9** by construction and is reproducible.
"""

from __future__ import annotations

import json
import math
import time
from dataclasses import asdict, dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any, Callable, Iterable, Sequence

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "Observation",
    "LinkKind",
    "CausalLink",
    "CausalGraph",
    "CausalReasoningEngine",
    "CausalError",
    "correlation",
    "partial_correlation",
    "MIN_OBSERVATIONS",
    "CORRELATION_THRESHOLD",
    "SCREEN_OFF_THRESHOLD",
]

DDR_ID = "D-06"
_FILE = "aether/agents/causal/causal_reasoning_engine.py"
CAPABILITY = "CAP_CAUSAL_WRITE"

DEFAULT_STORE = Path(__file__).resolve().parent / "causal_graph.jsonl"

MIN_OBSERVATIONS = 5
CORRELATION_THRESHOLD = 0.6
SCREEN_OFF_THRESHOLD = 0.25      # |partial r| below this = screened off
MAX_OBSERVATIONS = 10_000        # S2: bounded log


class CausalError(ValueError):
    """Causal operation refused."""


class LinkKind(str, Enum):
    CAUSAL = "causal"
    CONFOUNDED = "confounded"
    CORRELATED = "correlated"
    INDEPENDENT = "independent"


@dataclass(slots=True)
class Observation:
    """One sampled world state. ``intervened_on`` names a variable that was SET."""

    values: dict[str, float]
    intervened_on: str | None = None
    ts: float = field(default_factory=time.time)


@dataclass(slots=True)
class CausalLink:
    cause: str
    effect: str
    kind: LinkKind
    strength: float = 0.0
    confidence: float = 0.0
    confounder: str | None = None
    mechanism: str = ""

    def to_json(self) -> str:
        payload = asdict(self)
        payload["kind"] = self.kind.value
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def correlation(xs: Sequence[float], ys: Sequence[float]) -> float:
    """Pearson r. Returns 0.0 for degenerate input rather than raising."""
    n = len(xs)
    if n != len(ys) or n < 2:
        return 0.0
    mx = sum(xs) / n
    my = sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx <= 0.0 or syy <= 0.0:
        return 0.0                      # a constant column correlates with nothing
    return sxy / math.sqrt(sxx * syy)


def partial_correlation(
    xs: Sequence[float], ys: Sequence[float], zs: Sequence[float]
) -> float:
    """Correlation of x and y with z held fixed.

    This is the arithmetic that distinguishes a common cause from a direct one:
    if z explains the x–y association, this collapses toward 0.
    """
    rxy = correlation(xs, ys)
    rxz = correlation(xs, zs)
    ryz = correlation(ys, zs)
    denom = math.sqrt(max(0.0, (1 - rxz**2) * (1 - ryz**2)))
    if denom <= 1e-12:
        return 0.0
    return (rxy - rxz * ryz) / denom


class CausalGraph:
    """Links keyed by (cause, effect)."""

    def __init__(self) -> None:
        self._links: dict[tuple[str, str], CausalLink] = {}

    def put(self, link: CausalLink) -> CausalLink:
        self._links[(link.cause, link.effect)] = link
        return link

    def get(self, cause: str, effect: str) -> CausalLink | None:
        return self._links.get((cause, effect))

    def causal_links(self) -> list[CausalLink]:
        return sorted(
            (l for l in self._links.values() if l.kind is LinkKind.CAUSAL),
            key=lambda l: (-l.strength, l.cause, l.effect),
        )

    def all_links(self) -> list[CausalLink]:
        return sorted(self._links.values(), key=lambda l: (l.cause, l.effect))

    def effects_of(self, cause: str) -> list[CausalLink]:
        return [l for l in self.causal_links() if l.cause == cause]

    def __len__(self) -> int:
        return len(self._links)


class CausalReasoningEngine:
    """Infers causal structure from observations, conservatively."""

    def __init__(
        self,
        lockbox: Lockbox,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
        alignment_tap: Callable[[str, dict[str, Any]], None] | None = None,
        max_observations: int = MAX_OBSERVATIONS,
    ) -> None:
        self.lockbox = lockbox
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        # I-09: D-08 superalignment tap. Stub until D-08 lands; every loop step
        # emits through it so the real monitor can be dropped in unchanged.
        self.alignment_tap = alignment_tap
        self.max_observations = int(max_observations)
        self.graph = CausalGraph()
        self._observations: list[Observation] = []

    def _emit(self, step: str, detail: dict[str, Any]) -> None:
        if self.alignment_tap is not None:
            self.alignment_tap(f"{DDR_ID}.{step}", detail)

    # -- ingestion -----------------------------------------------------------
    def observe(self, observation: Observation) -> Observation:
        """Record one observation. Bounded (S2); a drop is logged (S12)."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not observation.values:
            raise CausalError("an observation needs at least one variable")
        if len(self._observations) >= self.max_observations:
            dropped = self._observations.pop(0)
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="causal.observation_dropped",
                gate_status="dropped", reason="observation window full",
                dropped_ts=dropped.ts, window=self.max_observations,
            )
        self._observations.append(observation)
        self._emit("observe", {"vars": sorted(observation.values)})
        return observation

    def observe_many(self, observations: Iterable[Observation]) -> int:
        count = 0
        for obs in observations:
            self.observe(obs)
            count += 1
        return count

    @property
    def observations(self) -> tuple[Observation, ...]:
        return tuple(self._observations)

    def variables(self) -> tuple[str, ...]:
        names: set[str] = set()
        for obs in self._observations:
            names.update(obs.values)
        return tuple(sorted(names))

    def _series(self, name: str, rows: Sequence[Observation]) -> list[float] | None:
        """Column for ``name``, or None if any row lacks it."""
        out: list[float] = []
        for obs in rows:
            if name not in obs.values:
                return None
            out.append(float(obs.values[name]))
        return out

    # -- inference -----------------------------------------------------------
    def infer(self, cause: str, effect: str) -> CausalLink:
        """Classify the cause→effect relation. Conservative by construction."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        if cause == effect:
            raise CausalError("a variable cannot cause itself")

        rows = [o for o in self._observations if cause in o.values and effect in o.values]
        if len(rows) < MIN_OBSERVATIONS:
            link = CausalLink(
                cause, effect, LinkKind.INDEPENDENT, 0.0, 0.0,
                mechanism=f"only {len(rows)} joint observations; "
                          f"{MIN_OBSERVATIONS} required",
            )
            return self._record(link)

        xs = self._series(cause, rows) or []
        ys = self._series(effect, rows) or []
        r = correlation(xs, ys)

        # 1. Interventional evidence outranks everything: if `cause` was SET and
        #    `effect` still tracks it, no unobserved common cause explains that.
        interventional = [o for o in rows if o.intervened_on == cause]
        if len(interventional) >= MIN_OBSERVATIONS:
            ixs = self._series(cause, interventional) or []
            iys = self._series(effect, interventional) or []
            ir = correlation(ixs, iys)
            if abs(ir) >= CORRELATION_THRESHOLD:
                return self._record(
                    CausalLink(
                        cause, effect, LinkKind.CAUSAL, round(abs(ir), 4),
                        confidence=round(min(1.0, len(interventional) / 10.0), 4),
                        mechanism=f"interventional: {len(interventional)} do({cause}) "
                                  f"samples, r={ir:.3f}",
                    )
                )

        if abs(r) < CORRELATION_THRESHOLD:
            return self._record(
                CausalLink(cause, effect, LinkKind.INDEPENDENT, round(abs(r), 4),
                           mechanism=f"r={r:.3f} below {CORRELATION_THRESHOLD}")
            )

        # 2. Screening-off: does some observed Z explain the association?
        for z in self.variables():
            if z in (cause, effect):
                continue
            zs = self._series(z, rows)
            if zs is None:
                continue
            pr = partial_correlation(xs, ys, zs)
            if abs(pr) < SCREEN_OFF_THRESHOLD:
                return self._record(
                    CausalLink(
                        cause, effect, LinkKind.CONFOUNDED, round(abs(r), 4),
                        confidence=round(1.0 - abs(pr), 4), confounder=z,
                        mechanism=f"r={r:.3f} collapses to {pr:.3f} given {z}",
                    )
                )

        # 3. Correlated, survives screening, but never intervened on. That is
        #    NOT a causal claim — it is the honest verdict for observational data.
        return self._record(
            CausalLink(
                cause, effect, LinkKind.CORRELATED, round(abs(r), 4),
                confidence=round(abs(r), 4),
                mechanism=f"r={r:.3f} survives screening; no intervention on {cause}",
            )
        )

    def infer_all(self) -> list[CausalLink]:
        names = self.variables()
        out: list[CausalLink] = []
        for a in names:
            for b in names:
                if a != b:
                    out.append(self.infer(a, b))
        return out

    def _record(self, link: CausalLink) -> CausalLink:
        self.graph.put(link)
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(link.to_json() + "\n")
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="causal.infer", gate_status=link.kind.value,
            cause=link.cause, effect=link.effect, strength=link.strength,
            confounder=link.confounder,
        )
        self._emit("infer", {"cause": link.cause, "effect": link.effect,
                             "kind": link.kind.value})
        return link

    # -- intervention prediction --------------------------------------------
    def predict_intervention(self, variable: str) -> list[CausalLink]:
        """do(variable): which effects follow, per CAUSAL links only.

        Correlated and confounded links are deliberately excluded — acting on a
        confounded link is the practical harm this engine exists to prevent.
        """
        self.lockbox.require(DDR_ID, CAPABILITY)
        effects = self.graph.effects_of(variable)
        self._emit("predict", {"variable": variable, "effects": len(effects)})
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="causal.predict", gate_status="ok",
            variable=variable, effects=[l.effect for l in effects],
        )
        return effects
