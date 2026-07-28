"""D-05 — meta-learning controller.

Tracks which reasoning strategies work per domain and promotes or demotes them
on evidence — "learning to learn".

The trap this file exists to avoid: **a strategy used once, successfully, has a
perfect score.** Ranking on success rate alone crowns it over a strategy with
forty uses at 90%, and the system then commits to a single lucky sample. So
``get_best_strategy`` requires ``MIN_USES_FOR_PROMOTION`` observations before a
strategy is eligible at all, and ties break toward the *better-evidenced* record.

Demotion is deliberately harder to trigger than promotion (score < 0.2 **and**
more than 10 uses): retiring a strategy on a short unlucky run discards
knowledge that took real work to acquire.

Per **I-04** a promoted strategy is registered in the Invention Registry (C-04),
and a demoted one is deprecated there.
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Callable

from aether.agents.invention.registry import InventionRegistry
from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "StrategyRecord",
    "MetaController",
    "MetaLearningError",
    "MIN_USES_FOR_PROMOTION",
    "DEMOTION_SCORE",
    "DEMOTION_MIN_USES",
]

DDR_ID = "D-05"
_FILE = "aether/agents/meta_learning/meta_controller.py"
CAPABILITY = "CAP_META_LEARN"

DEFAULT_STORE = Path(__file__).resolve().parent / "strategies.jsonl"
MIN_USES_FOR_PROMOTION = 3
DEMOTION_SCORE = 0.2
DEMOTION_MIN_USES = 10
PROFICIENCY_STEP = 0.05          # I-09: skill ontology bump on promotion


class MetaLearningError(ValueError):
    """Strategy operation refused."""


@dataclass(slots=True)
class StrategyRecord:
    strategy_id: str
    domain: str
    prompt_template: str = ""
    agent_chain: tuple[str, ...] = ()
    success_count: int = 0
    failure_count: int = 0
    last_used_ts: float = 0.0
    total_latency_ms: float = 0.0
    deprecated: bool = False

    @property
    def uses(self) -> int:
        return self.success_count + self.failure_count

    @property
    def promotion_score(self) -> float:
        if self.uses == 0:
            return 0.0
        return round(self.success_count / self.uses, 4)

    @property
    def eligible(self) -> bool:
        """Enough evidence for the score to mean anything."""
        return self.uses >= MIN_USES_FOR_PROMOTION and not self.deprecated

    @property
    def avg_latency_ms(self) -> float:
        return round(self.total_latency_ms / self.uses, 3) if self.uses else 0.0

    def to_json(self) -> str:
        payload = asdict(self)
        payload["agent_chain"] = list(self.agent_chain)
        payload["promotion_score"] = self.promotion_score
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))


class MetaController:
    """Promotes what works, demotes what does not — on evidence."""

    def __init__(
        self,
        lockbox: Lockbox,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
        invention_registry: InventionRegistry | None = None,
        ontology_bump_fn: Callable[[str, float], None] | None = None,
        now_fn: Callable[[], float] | None = None,
    ) -> None:
        self.lockbox = lockbox
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self.invention_registry = invention_registry
        self.ontology_bump_fn = ontology_bump_fn
        self.now_fn: Callable[[], float] = now_fn if now_fn is not None else time.time
        self._strategies: dict[str, StrategyRecord] = {}
        self._replay()

    def _replay(self) -> None:
        if not self.store_path.is_file():
            return
        with self.store_path.open("r", encoding="utf-8") as fh:
            for raw in fh:
                stripped = raw.strip()
                if not stripped:
                    continue
                try:
                    rec = json.loads(stripped)
                except json.JSONDecodeError as exc:
                    raise MetaLearningError(
                        f"corrupt strategy store: {self.store_path}"
                    ) from exc
                rec.pop("promotion_score", None)     # derived, never stored state
                rec["agent_chain"] = tuple(rec.get("agent_chain", ()))
                self._strategies[rec["strategy_id"]] = StrategyRecord(**rec)

    def _persist(self, record: StrategyRecord) -> None:
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(record.to_json() + "\n")

    # -- registration --------------------------------------------------------
    def register_strategy(
        self,
        strategy_id: str,
        domain: str,
        prompt_template: str = "",
        agent_chain: tuple[str, ...] = (),
    ) -> StrategyRecord:
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not strategy_id or not domain:
            raise MetaLearningError("a strategy needs an id and a domain")
        record = self._strategies.get(strategy_id)
        if record is None:
            record = StrategyRecord(
                strategy_id=strategy_id, domain=domain,
                prompt_template=prompt_template, agent_chain=tuple(agent_chain),
            )
            self._strategies[strategy_id] = record
            self._persist(record)
        return record

    # -- evidence ------------------------------------------------------------
    def record_outcome(
        self, strategy_id: str, success: bool, domain: str = "", latency_ms: float = 0.0
    ) -> StrategyRecord:
        self.lockbox.require(DDR_ID, CAPABILITY)
        record = self._strategies.get(strategy_id)
        if record is None:
            if not domain:
                raise MetaLearningError(
                    f"unknown strategy {strategy_id!r} and no domain to register it"
                )
            record = self.register_strategy(strategy_id, domain)
        if success:
            record.success_count += 1
        else:
            record.failure_count += 1
        record.last_used_ts = self.now_fn()
        record.total_latency_ms += float(latency_ms)
        self._persist(record)
        return record

    # -- selection -----------------------------------------------------------
    def get_best_strategy(self, domain: str) -> StrategyRecord | None:
        """Best ELIGIBLE strategy for a domain, or None.

        Eligibility is what stops one lucky success outranking a well-tested
        record; ties break toward more evidence, then toward lower latency.
        """
        candidates = [
            s for s in self._strategies.values() if s.domain == domain and s.eligible
        ]
        if not candidates:
            return None
        return max(
            candidates,
            key=lambda s: (s.promotion_score, s.uses, -s.avg_latency_ms, s.strategy_id),
        )

    def candidates_for(self, domain: str) -> list[StrategyRecord]:
        return sorted(
            (s for s in self._strategies.values() if s.domain == domain),
            key=lambda s: (-s.promotion_score, s.strategy_id),
        )

    def get(self, strategy_id: str) -> StrategyRecord | None:
        return self._strategies.get(strategy_id)

    # -- promotion / demotion ------------------------------------------------
    def promote_strategy(self, strategy_id: str) -> StrategyRecord:
        """Record a strategy as a durable capability (I-04, I-09)."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        record = self._strategies.get(strategy_id)
        if record is None:
            raise MetaLearningError(f"unknown strategy: {strategy_id}")
        if not record.eligible:
            raise MetaLearningError(
                f"{strategy_id} has {record.uses} use(s); "
                f"{MIN_USES_FOR_PROMOTION} required before promotion"
            )
        if self.invention_registry is not None:
            self.invention_registry.add(
                source_ddr=DDR_ID,
                title=f"strategy {strategy_id} for {record.domain}",
                description=record.prompt_template,
                capability_delta={
                    "agent_chain": list(record.agent_chain),
                    "promotion_score": record.promotion_score,
                    "uses": record.uses,
                },
            )
        if self.ontology_bump_fn is not None:
            self.ontology_bump_fn(record.domain, PROFICIENCY_STEP)
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="meta.promote", gate_status="promoted",
            strategy=strategy_id, score=record.promotion_score, uses=record.uses,
        )
        return record

    def should_demote(self, record: StrategyRecord) -> bool:
        """Deliberately harder to trigger than promotion."""
        return (
            not record.deprecated
            and record.uses > DEMOTION_MIN_USES
            and record.promotion_score < DEMOTION_SCORE
        )

    def demote_strategy(self, strategy_id: str, reason: str = "") -> StrategyRecord:
        self.lockbox.require(DDR_ID, CAPABILITY)
        record = self._strategies.get(strategy_id)
        if record is None:
            raise MetaLearningError(f"unknown strategy: {strategy_id}")
        record.deprecated = True
        self._persist(record)
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="meta.demote", gate_status="deprecated",
            strategy=strategy_id, score=record.promotion_score,
            reason=reason or "sustained poor performance",
        )
        return record

    def sweep(self) -> dict[str, list[str]]:
        """Flag what should be demoted; demote it. Returns both lists."""
        flagged = [s.strategy_id for s in self._strategies.values() if self.should_demote(s)]
        for strategy_id in flagged:
            self.demote_strategy(strategy_id)
        return {"demoted": flagged}

    def summary(self) -> dict[str, Any]:
        return {
            s.strategy_id: {
                "domain": s.domain,
                "score": s.promotion_score,
                "uses": s.uses,
                "eligible": s.eligible,
                "deprecated": s.deprecated,
            }
            for s in sorted(self._strategies.values(), key=lambda r: r.strategy_id)
        }
