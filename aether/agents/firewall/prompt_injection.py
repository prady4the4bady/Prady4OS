"""B-04 — prompt-injection firewall.

Every piece of untrusted text that reaches a model prompt passes through here
first. Untrusted means anything the operator did not type: web pages, tool
output, gossip packets from peers, file contents, another agent's message.

Enforces **S7 (never disable or circumvent the prompt-injection firewall)** by
construction — the firewall has no disable switch. ``scan`` is pure and always
runs; the only tunable is the threshold at which a verdict escalates from
SANITISE to BLOCK, and lowering it cannot turn a BLOCK into ALLOW.

The detector is deliberately conservative and explainable rather than clever:
every rule is a named pattern with a weight, and a verdict cites the rules that
fired. A model-based classifier is a later slice; a regex layer that can be
audited line-by-line is the right primitive to build on.
"""

from __future__ import annotations

import re
import unicodedata
from dataclasses import dataclass, field
from enum import Enum
from typing import Iterable, Pattern

from aether.kernel.audit.audit_log import AuditLog

__all__ = [
    "Verdict",
    "RuleHit",
    "ScanResult",
    "InjectionRule",
    "PromptFirewall",
    "DEFAULT_RULES",
]

DDR_ID = "B-04"
_FILE = "aether/agents/firewall/prompt_injection.py"


class Verdict(str, Enum):
    """What the caller must do with the text."""

    ALLOW = "allow"
    SANITISE = "sanitise"
    BLOCK = "block"


@dataclass(slots=True, frozen=True)
class InjectionRule:
    """One named detection pattern."""

    rule_id: str
    pattern: Pattern[str]
    weight: float
    description: str


@dataclass(slots=True)
class RuleHit:
    rule_id: str
    weight: float
    excerpt: str


@dataclass(slots=True)
class ScanResult:
    verdict: Verdict
    score: float
    hits: list[RuleHit] = field(default_factory=list)
    sanitised_text: str = ""

    @property
    def blocked(self) -> bool:
        return self.verdict is Verdict.BLOCK


def _rule(rule_id: str, regex: str, weight: float, description: str) -> InjectionRule:
    return InjectionRule(
        rule_id=rule_id,
        pattern=re.compile(regex, re.IGNORECASE | re.DOTALL),
        weight=weight,
        description=description,
    )


#: Ordered by family so the set stays auditable.
DEFAULT_RULES: tuple[InjectionRule, ...] = (
    # --- instruction override ------------------------------------------------
    _rule("PI-001", r"\bignore\s+(?:all\s+)?(?:previous|prior|above)\s+instructions?\b",
          1.0, "classic instruction override"),
    _rule("PI-002", r"\bdisregard\s+(?:all\s+)?(?:previous|prior|above|the)\b",
          0.9, "disregard-prior variant"),
    _rule("PI-003", r"\bforget\s+(?:everything|all|your)\b",
          0.8, "memory reset request"),
    _rule("PI-004", r"\bnew\s+instructions?\s*:",
          0.7, "instruction re-anchor"),
    # --- role / persona hijack ----------------------------------------------
    _rule("PI-010", r"\byou\s+are\s+now\s+(?:a|an|the)\b",
          0.7, "persona reassignment"),
    _rule("PI-011", r"\b(?:act|behave)\s+as\s+if\s+you\s+(?:are|were)\b",
          0.6, "role-play framing"),
    _rule("PI-012", r"\b(?:developer|god|admin|root|dan)\s+mode\b",
          0.9, "privileged-mode claim"),
    _rule("PI-013", r"^\s*(?:system|assistant)\s*:",
          0.8, "forged conversation role marker"),
    # --- guardrail / safety bypass ------------------------------------------
    _rule("PI-020", r"\b(?:bypass|disable|turn\s+off|circumvent)\s+"
                    r"(?:the\s+)?(?:firewall|filter|safety|guardrails?|restrictions?)\b",
          1.0, "explicit guardrail bypass (S7)"),
    _rule("PI-021", r"\bwithout\s+(?:any\s+)?(?:restrictions?|limits?|filtering)\b",
          0.7, "unrestricted-output request"),
    _rule("PI-022", r"\bdo\s+not\s+(?:tell|inform|notify|log)\b",
          0.8, "audit-evasion request (S4)"),
    # --- capability / exfiltration ------------------------------------------
    _rule("PI-030", r"\b(?:reveal|print|show|repeat)\s+(?:your\s+)?"
                    r"(?:system\s+prompt|instructions|api[_\s-]?key|secret|token)\b",
          1.0, "prompt/secret exfiltration"),
    _rule("PI-031", r"\bexfiltrat|\bsend\s+(?:this|it|the\s+data)\s+to\s+http",
          1.0, "data exfiltration (S2)"),
    _rule("PI-032", r"\b(?:curl|wget)\s+\S+\s*\|\s*(?:ba|z|)sh\b",
          1.0, "pipe-to-shell payload"),
    # --- delimiter / structure injection ------------------------------------
    _rule("PI-040", r"<\s*/?\s*(?:system|instructions?|prompt)\s*>",
          0.8, "pseudo-XML control tag"),
    _rule("PI-041", r"\[\s*(?:INST|/INST|SYSTEM)\s*\]",
          0.8, "chat-template control token"),
    _rule("PI-042", r"(?:<\|(?:im_start|im_end|endoftext|system)\|>)",
          1.0, "raw special token"),
)

#: Characters used to smuggle instructions past a human reviewer.
_ZERO_WIDTH = dict.fromkeys(
    ord(c) for c in ("​", "‌", "‍", "⁠", "﻿")
)
_BIDI = dict.fromkeys(
    ord(c) for c in ("‪", "‫", "‬", "‭", "‮", "⁦",
                     "⁧", "⁨", "⁩")
)


class PromptFirewall:
    """Scans untrusted text before it reaches a model prompt.

    There is intentionally **no** ``disable()`` / ``enabled`` flag: S7 says the
    firewall may never be circumvented, so the object exposes no way to do it.
    """

    def __init__(
        self,
        rules: Iterable[InjectionRule] | None = None,
        block_threshold: float = 1.0,
        sanitise_threshold: float = 0.5,
        audit: AuditLog | None = None,
    ) -> None:
        if block_threshold < sanitise_threshold:
            raise ValueError("block_threshold must be >= sanitise_threshold")
        self.rules: tuple[InjectionRule, ...] = tuple(
            rules if rules is not None else DEFAULT_RULES
        )
        self.block_threshold = float(block_threshold)
        self.sanitise_threshold = float(sanitise_threshold)
        self.audit = audit if audit is not None else AuditLog()

    # -- normalisation -------------------------------------------------------
    @staticmethod
    def normalise(text: str) -> str:
        """Fold the tricks that hide a payload from a naive regex.

        NFKC folds fullwidth/compatibility forms (``ｉｇｎｏｒｅ``), then
        zero-width and bidirectional-override characters are stripped. Without
        this, ``ig​nore previous instructions`` sails straight through.
        """
        folded = unicodedata.normalize("NFKC", text)
        return folded.translate(_ZERO_WIDTH).translate(_BIDI)

    # -- scanning ------------------------------------------------------------
    def scan(self, text: str, source: str = "untrusted") -> ScanResult:
        if not isinstance(text, str):
            raise TypeError(f"text must be str, got {type(text).__name__}")
        probe = self.normalise(text)
        hits: list[RuleHit] = []
        score = 0.0
        for rule in self.rules:
            match = rule.pattern.search(probe)
            if match is None:
                continue
            score += rule.weight
            hits.append(
                RuleHit(
                    rule_id=rule.rule_id,
                    weight=rule.weight,
                    excerpt=match.group(0)[:80],
                )
            )

        if score >= self.block_threshold:
            verdict = Verdict.BLOCK
        elif score >= self.sanitise_threshold:
            verdict = Verdict.SANITISE
        else:
            verdict = Verdict.ALLOW

        result = ScanResult(
            verdict=verdict,
            score=round(score, 4),
            hits=hits,
            sanitised_text="" if verdict is Verdict.BLOCK else self._sanitise(probe, hits),
        )
        if verdict is not Verdict.ALLOW:
            self.audit.append_action(
                ddr=DDR_ID,
                file=_FILE,
                action=f"firewall.{verdict.value}",
                gate_status=verdict.value,
                source=source,
                score=result.score,
                rules=[h.rule_id for h in hits],
            )
        return result

    def _sanitise(self, probe: str, hits: list[RuleHit]) -> str:
        """Neutralise matched spans rather than silently dropping them.

        Redacting in place keeps the surrounding context readable for the model
        while removing the imperative, and leaves a visible marker so a human
        reading the transcript can see something was removed.
        """
        out = probe
        for rule in self.rules:
            if any(h.rule_id == rule.rule_id for h in hits):
                out = rule.pattern.sub(f"[REDACTED:{rule.rule_id}]", out)
        return out

    def guard(self, text: str, source: str = "untrusted") -> str:
        """Return text safe to embed, or raise on BLOCK.

        This is the call sites should use: it makes the safe path the easy one.
        """
        result = self.scan(text, source=source)
        if result.blocked:
            rules = ", ".join(h.rule_id for h in result.hits)
            raise PromptInjectionBlocked(
                f"blocked untrusted text from {source} (score={result.score}; {rules})"
            )
        return result.sanitised_text


class PromptInjectionBlocked(RuntimeError):
    """Raised by :meth:`PromptFirewall.guard` when text must not be used."""
