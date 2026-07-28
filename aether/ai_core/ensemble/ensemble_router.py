"""D-03 — multi-model ensemble router.

Routes inference to the best available model for a task, decoupling AETHER from
any single backend so a stronger model can be dropped in without touching call
sites. Per **I-08** this becomes the only path to inference.

Three decisions worth stating:

* **A quality floor is a floor, not a preference.** If no model meets it, the
  router escalates to Offline Safety Review rather than silently serving the
  best of a bad set. Degrading quietly is how a system starts producing
  confident nonsense after a model is unregistered.
* **Domain match is a gate, not a weight.** A model that does not cover the
  domain is excluded, not merely penalised — otherwise a very fast, very cheap,
  entirely unsuitable model wins on aggregate score.
* **Stats update from observed behaviour.** ``update_stats`` feeds an EWMA of
  real latency and quality back into routing, so a model that degrades in
  production stops being chosen without anyone editing a config.
"""

from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Callable, Literal

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.lockbox.cap_sovereign import Lockbox

__all__ = [
    "ModelProfile",
    "EnsembleRouter",
    "RoutingError",
    "NoModelAvailable",
    "Provider",
    "EWMA_ALPHA",
    "estimate_tokens",
]

DDR_ID = "D-03"
_FILE = "aether/ai_core/ensemble/ensemble_router.py"
CAPABILITY = "CAP_MODEL_ROUTE"

DEFAULT_STORE = Path(__file__).resolve().parent / "model_profiles.jsonl"
EWMA_ALPHA = 0.3
LATENCY_REFERENCE_MS = 2000.0

Provider = Literal["ollama", "openai", "anthropic", "custom"]


class RoutingError(ValueError):
    """Routing request malformed."""


class NoModelAvailable(RuntimeError):
    """No model satisfies the request."""


def estimate_tokens(text: str) -> int:
    """Approximate token count for audit accounting (I-04).

    A whitespace/4-chars heuristic, not a tokenizer. It is deliberately local:
    importing a real tokenizer would make the audit path depend on a model
    package being installed, and an audit entry that can fail to be written is
    worse than an approximate one. Callers that need exact counts should record
    the provider's reported usage instead.
    """
    if not text:
        return 0
    return max(1, (len(text) + 3) // 4)


@dataclass(slots=True)
class ModelProfile:
    model_id: str
    provider: Provider
    domains: tuple[str, ...] = ()
    avg_latency_ms: float = 0.0
    quality_score: float = 0.5
    cost_per_1k_tokens: float = 0.0
    available: bool = True
    observations: int = 0

    def __post_init__(self) -> None:
        if not 0.0 <= self.quality_score <= 1.0:
            raise RoutingError(
                f"{self.model_id}: quality_score {self.quality_score} out of range"
            )

    def covers(self, domain: str) -> bool:
        """A model with no declared domains is a generalist."""
        return not self.domains or domain in self.domains

    def to_json(self) -> str:
        return json.dumps(asdict(self), sort_keys=True, separators=(",", ":"))


class EnsembleRouter:
    """Chooses a model per request and learns from what happens."""

    def __init__(
        self,
        lockbox: Lockbox,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
        escalate_fn: Callable[[str, dict[str, Any]], None] | None = None,
    ) -> None:
        self.lockbox = lockbox
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self.escalate_fn = escalate_fn
        self._models: dict[str, ModelProfile] = {}
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
                    raise RoutingError(
                        f"corrupt model profile store: {self.store_path}"
                    ) from exc
                rec["domains"] = tuple(rec.get("domains", ()))
                self._models[rec["model_id"]] = ModelProfile(**rec)

    def _persist(self, profile: ModelProfile) -> None:
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(profile.to_json() + "\n")

    # -- registration --------------------------------------------------------
    def register_model(self, profile: ModelProfile) -> ModelProfile:
        self.lockbox.require(DDR_ID, CAPABILITY)
        self._models[profile.model_id] = profile
        self._persist(profile)
        return profile

    def unregister(self, model_id: str) -> None:
        profile = self._models.get(model_id)
        if profile is not None:
            profile.available = False
            self._persist(profile)

    @property
    def models(self) -> tuple[ModelProfile, ...]:
        return tuple(sorted(self._models.values(), key=lambda m: m.model_id))

    # -- routing -------------------------------------------------------------
    def score(self, profile: ModelProfile, latency_budget_ms: float) -> float:
        """Quality minus a latency penalty, both bounded to [0, 1]."""
        if latency_budget_ms <= 0:
            raise RoutingError("latency_budget_ms must be > 0")
        over = max(0.0, profile.avg_latency_ms - latency_budget_ms)
        penalty = min(1.0, over / LATENCY_REFERENCE_MS)
        return max(0.0, profile.quality_score - penalty)

    def route(
        self,
        task_domain: str,
        latency_budget_ms: float = 5000.0,
        quality_floor: float = 0.0,
    ) -> ModelProfile:
        """Pick the best model, or escalate when the floor cannot be met."""
        self.lockbox.require(DDR_ID, CAPABILITY)
        if not task_domain:
            raise RoutingError("task_domain must be non-empty")
        if not 0.0 <= quality_floor <= 1.0:
            raise RoutingError(f"quality_floor {quality_floor} out of range 0..1")

        # Domain coverage is a GATE. A model that does not cover the domain is
        # excluded outright, so a fast cheap unsuitable model cannot win on
        # aggregate score.
        eligible = [
            m
            for m in self._models.values()
            if m.available and m.covers(task_domain) and m.quality_score >= quality_floor
        ]
        if not eligible:
            context = {
                "domain": task_domain,
                "quality_floor": quality_floor,
                "candidates": len(self._models),
            }
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="route.no_model",
                gate_status="escalated", **context,
            )
            if self.escalate_fn is not None:
                self.escalate_fn(task_domain, context)
            raise NoModelAvailable(
                f"no model covers {task_domain!r} at quality >= {quality_floor}"
            )

        best = max(
            eligible,
            key=lambda m: (self.score(m, latency_budget_ms), -m.cost_per_1k_tokens, m.model_id),
        )
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="route.select", gate_status="ok",
            domain=task_domain, model=best.model_id,
            score=round(self.score(best, latency_budget_ms), 4),
            eligible=len(eligible),
        )
        return best

    # -- learning ------------------------------------------------------------
    def update_stats(
        self, model_id: str, actual_latency_ms: float, quality_rating: float
    ) -> ModelProfile:
        """Fold observed behaviour back into the profile via an EWMA.

        A model that degrades in production stops being chosen without anyone
        editing a config file.
        """
        profile = self._models.get(model_id)
        if profile is None:
            raise RoutingError(f"unknown model: {model_id}")
        if not 0.0 <= quality_rating <= 1.0:
            raise RoutingError(f"quality_rating {quality_rating} out of range 0..1")

        if profile.observations == 0:
            profile.avg_latency_ms = float(actual_latency_ms)
            profile.quality_score = float(quality_rating)
        else:
            profile.avg_latency_ms = (
                EWMA_ALPHA * float(actual_latency_ms)
                + (1 - EWMA_ALPHA) * profile.avg_latency_ms
            )
            profile.quality_score = (
                EWMA_ALPHA * float(quality_rating)
                + (1 - EWMA_ALPHA) * profile.quality_score
            )
        profile.observations += 1
        profile.avg_latency_ms = round(profile.avg_latency_ms, 3)
        profile.quality_score = round(profile.quality_score, 4)
        self._persist(profile)
        return profile

    def infer(
        self,
        prompt: str,
        task_domain: str,
        runners: dict[str, Callable[[str, ModelProfile], str]],
        latency_budget_ms: float = 5000.0,
        quality_floor: float = 0.0,
    ) -> tuple[str, ModelProfile]:
        """Route then run, recording observed latency and returning both.

        **I-04**: every completed call writes one ``route.call`` audit entry
        carrying ``token_count`` and ``latency_ms``. ``route.select`` alone
        records only that a model was *chosen* — without the per-call cost the
        log says inference happened but not what it cost, so no budget or
        latency-regression question can be answered from it afterwards.
        """
        profile = self.route(task_domain, latency_budget_ms, quality_floor)
        runner = runners.get(profile.model_id)
        if runner is None:
            raise RoutingError(f"no runner registered for {profile.model_id}")
        started = time.perf_counter()
        text = runner(prompt, profile)
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        self.update_stats(profile.model_id, elapsed_ms, profile.quality_score)
        token_count = estimate_tokens(prompt) + estimate_tokens(text)
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="route.call", gate_status="ok",
            domain=task_domain, model=profile.model_id,
            token_count=token_count, latency_ms=round(elapsed_ms, 3),
            cost=round(token_count / 1000.0 * profile.cost_per_1k_tokens, 6),
        )
        return text, profile
