"""B-09 — Ollama model manager.

Owns the connection to a local Ollama daemon: which models exist, which are
loaded, and the single ``infer`` entry point every other DDR uses.

Note on placement: the layout says ``ai-core/``, but a hyphen is not a legal
Python identifier, so ``import aether.ai-core.model_manager`` is a syntax error.
The package is therefore ``aether/ai_core/`` — same position in the tree, legal
to import.

Design points that matter downstream:

* **Transport is injected.** ``ModelManager`` takes a ``transport`` callable, so
  every consumer's gate can run without a live daemon and without monkeypatching
  the network. A default HTTP transport is provided for real use.
* **Every inference is audited** with model, latency and token estimate — the
  Meta-Learning Controller (D-05) and Cognitive Load Balancer (D-13) both read
  those records rather than instrumenting call sites separately.
* **Untrusted prompt material goes through the firewall.** ``infer`` accepts an
  ``untrusted`` argument that is scanned by B-04 before it is concatenated, so
  the S7 path is available at the one place prompts are actually built.

D-03 (Ensemble Router) will later become the only path to inference; this class
stays the Ollama-specific backend behind it.
"""

from __future__ import annotations

import json
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Callable

from aether.agents.firewall.prompt_injection import PromptFirewall
from aether.kernel.audit.audit_log import AuditLog

__all__ = [
    "ModelInfo",
    "InferenceResult",
    "ModelManager",
    "ModelUnavailable",
    "Transport",
    "http_transport",
]

DDR_ID = "B-09"
_FILE = "aether/ai_core/model_manager/manager.py"

DEFAULT_HOST = "http://127.0.0.1:11434"

#: A transport takes (host, endpoint, payload) and returns the decoded response.
Transport = Callable[[str, str, dict[str, Any]], dict[str, Any]]


class ModelUnavailable(RuntimeError):
    """Raised when no usable model can serve a request."""


@dataclass(slots=True)
class ModelInfo:
    name: str
    size_bytes: int = 0
    family: str = ""
    loaded: bool = False


@dataclass(slots=True)
class InferenceResult:
    model: str
    text: str
    latency_ms: float
    prompt_chars: int
    completion_chars: int
    meta: dict[str, Any] = field(default_factory=dict)

    @property
    def approx_tokens(self) -> int:
        """Rough token estimate: ~4 chars/token. Used for load accounting."""
        return max(1, (self.prompt_chars + self.completion_chars) // 4)


def http_transport(host: str, endpoint: str, payload: dict[str, Any]) -> dict[str, Any]:
    """Default transport — POST JSON to the Ollama daemon."""
    url = f"{host.rstrip('/')}/{endpoint.lstrip('/')}"
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url, data=body, headers={"Content-Type": "application/json"}, method="POST"
    )
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:  # noqa: S310 - fixed host
            return json.loads(resp.read().decode("utf-8"))
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        raise ModelUnavailable(f"ollama request failed: {exc}") from exc


class ModelManager:
    """Manages local Ollama models and serves inference."""

    def __init__(
        self,
        host: str = DEFAULT_HOST,
        transport: Transport | None = None,
        firewall: PromptFirewall | None = None,
        audit: AuditLog | None = None,
        default_model: str = "llama3",
    ) -> None:
        self.host = host
        self.transport: Transport = transport if transport is not None else http_transport
        self.firewall = firewall if firewall is not None else PromptFirewall()
        self.audit = audit if audit is not None else AuditLog()
        self.default_model = default_model
        self._models: dict[str, ModelInfo] = {}

    # -- model inventory -----------------------------------------------------
    def register_model(self, info: ModelInfo) -> ModelInfo:
        self._models[info.name] = info
        return info

    def list_models(self) -> list[ModelInfo]:
        return sorted(self._models.values(), key=lambda m: m.name)

    def refresh(self) -> list[ModelInfo]:
        """Ask the daemon what it has."""
        data = self.transport(self.host, "api/tags", {})
        for entry in data.get("models", []):
            name = entry.get("name", "")
            if not name:
                continue
            self.register_model(
                ModelInfo(
                    name=name,
                    size_bytes=int(entry.get("size", 0)),
                    family=str(entry.get("details", {}).get("family", "")),
                    loaded=True,
                )
            )
        return self.list_models()

    def available(self, name: str) -> bool:
        return name in self._models

    # -- inference -----------------------------------------------------------
    def infer(
        self,
        prompt: str,
        model: str | None = None,
        temperature: float = 0.2,
        untrusted: str | None = None,
        source: str = "internal",
    ) -> InferenceResult:
        """Run one completion.

        ``untrusted`` is scanned by the B-04 firewall before being appended to
        ``prompt``. Building the prompt is the one place where untrusted text
        becomes an instruction, so the guard belongs here rather than at each
        of the ~20 call sites that will exist by D-15.
        """
        chosen = model or self.default_model
        if self._models and chosen not in self._models:
            raise ModelUnavailable(f"model not available: {chosen}")

        full_prompt = prompt
        if untrusted:
            safe = self.firewall.guard(untrusted, source=source)
            full_prompt = f"{prompt}\n\n<untrusted-content>\n{safe}\n</untrusted-content>"

        started = time.perf_counter()
        response = self.transport(
            self.host,
            "api/generate",
            {
                "model": chosen,
                "prompt": full_prompt,
                "stream": False,
                "options": {"temperature": float(temperature)},
            },
        )
        latency_ms = (time.perf_counter() - started) * 1000.0
        text = str(response.get("response", ""))
        result = InferenceResult(
            model=chosen,
            text=text,
            latency_ms=round(latency_ms, 3),
            prompt_chars=len(full_prompt),
            completion_chars=len(text),
            meta={k: v for k, v in response.items() if k != "response"},
        )
        self.audit.append_action(
            ddr=DDR_ID,
            file=_FILE,
            action="model.infer",
            gate_status="ok",
            model=chosen,
            latency_ms=result.latency_ms,
            approx_tokens=result.approx_tokens,
            source=source,
        )
        return result

    def infer_json(
        self,
        prompt: str,
        model: str | None = None,
        temperature: float = 0.2,
        untrusted: str | None = None,
        source: str = "internal",
    ) -> dict[str, Any]:
        """Inference that must return a JSON object.

        Many DDRs (C-07, D-01, D-07, D-14) ask the model for structured output.
        Models wrap JSON in prose or fences often enough that every one of those
        call sites would otherwise reimplement the same salvage logic.
        """
        result = self.infer(
            prompt, model=model, temperature=temperature,
            untrusted=untrusted, source=source,
        )
        return self.extract_json(result.text)

    @staticmethod
    def extract_json(text: str) -> dict[str, Any]:
        """Pull the first JSON object out of a model response."""
        stripped = text.strip()
        if stripped.startswith("```"):
            lines = [ln for ln in stripped.splitlines() if not ln.startswith("```")]
            stripped = "\n".join(lines).strip()
        try:
            parsed = json.loads(stripped)
        except json.JSONDecodeError:
            start = stripped.find("{")
            end = stripped.rfind("}")
            if start == -1 or end <= start:
                raise ValueError(f"no JSON object in model response: {text[:120]!r}")
            try:
                parsed = json.loads(stripped[start : end + 1])
            except json.JSONDecodeError as exc:
                raise ValueError(
                    f"malformed JSON in model response: {text[:120]!r}"
                ) from exc
        if not isinstance(parsed, dict):
            raise ValueError(f"expected a JSON object, got {type(parsed).__name__}")
        return parsed
