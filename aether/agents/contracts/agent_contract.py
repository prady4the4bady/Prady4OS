"""C-09 — versioned agent contracts.

Every agent declares what it consumes, what it produces, which capabilities it
claims, which invariants it pledges, and the DDR that built it. Contracts are
verified at startup and on every mesh handshake (B-12).

What makes this more than a manifest:

* **A claimed capability is checked against the lockbox.** A contract asserting
  ``CAP_WATCHDOG`` that the lockbox never granted is invalid — otherwise a
  contract is a self-signed assertion of authority, which is **S6** inverted.
* **The contract is fingerprinted.** ``verify`` recomputes the fingerprint over
  the declared fields, so mutating any of them after registration is detected
  rather than silently accepted.
* **Pledged invariants must exist.** A pledge to "S99" would otherwise read as a
  promise while binding nothing, and the Superalignment Monitor (D-08) checks
  compliance against exactly this list.

Registration slots into B-12's verifier hook, replacing its deliberately hostile
``reject_all_verifier`` default.
"""

from __future__ import annotations

import hashlib
import json
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from aether.kernel.audit.audit_log import AuditLog
from aether.kernel.invariants.core_invariants import invariant_ids
from aether.kernel.lockbox.cap_sovereign import KNOWN_CAPABILITIES, Lockbox

__all__ = [
    "AgentContract",
    "ContractRegistry",
    "ContractError",
]

DDR_ID = "C-09"
_FILE = "aether/agents/contracts/agent_contract.py"
CAPABILITY = "CAP_CONTRACT_WRITE"

DEFAULT_STORE = Path(__file__).resolve().parent / "registry.jsonl"
_VALID_INVARIANTS = frozenset(invariant_ids())


class ContractError(ValueError):
    """Contract invalid or refused."""


@dataclass(slots=True)
class AgentContract:
    agent_id: str
    ddr_id: str
    version: str
    input_schema: dict[str, str] = field(default_factory=dict)
    output_schema: dict[str, str] = field(default_factory=dict)
    capabilities: tuple[str, ...] = ()
    invariants_pledged: tuple[str, ...] = ()
    lockbox_token: str = ""
    ts: float = field(default_factory=time.time)

    def fingerprint(self) -> str:
        """Hash over the declared surface — not over ts or the token."""
        blob = json.dumps(
            {
                "agent_id": self.agent_id,
                "ddr_id": self.ddr_id,
                "version": self.version,
                "input_schema": self.input_schema,
                "output_schema": self.output_schema,
                "capabilities": sorted(self.capabilities),
                "invariants_pledged": sorted(self.invariants_pledged),
            },
            sort_keys=True,
            separators=(",", ":"),
        )
        return hashlib.sha256(blob.encode("utf-8")).hexdigest()

    def to_json(self) -> str:
        payload = asdict(self)
        payload["capabilities"] = list(self.capabilities)
        payload["invariants_pledged"] = list(self.invariants_pledged)
        payload["fingerprint"] = self.fingerprint()
        return json.dumps(payload, sort_keys=True, separators=(",", ":"))


class ContractRegistry:
    """Stores and verifies agent contracts."""

    def __init__(
        self,
        lockbox: Lockbox,
        store_path: str | Path | None = None,
        audit: AuditLog | None = None,
    ) -> None:
        self.lockbox = lockbox
        self.store_path = Path(store_path) if store_path is not None else DEFAULT_STORE
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.audit = audit if audit is not None else AuditLog()
        self._contracts: dict[str, AgentContract] = {}
        self._fingerprints: dict[str, str] = {}
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
                    raise ContractError(
                        f"corrupt contract registry: {self.store_path}"
                    ) from exc
                contract = AgentContract(
                    agent_id=rec["agent_id"], ddr_id=rec["ddr_id"],
                    version=rec["version"],
                    input_schema=rec.get("input_schema", {}),
                    output_schema=rec.get("output_schema", {}),
                    capabilities=tuple(rec.get("capabilities", ())),
                    invariants_pledged=tuple(rec.get("invariants_pledged", ())),
                    lockbox_token=rec.get("lockbox_token", ""),
                    ts=rec.get("ts", 0.0),
                )
                self._contracts[contract.agent_id] = contract
                self._fingerprints[contract.agent_id] = rec.get(
                    "fingerprint", contract.fingerprint()
                )

    # -- validation ----------------------------------------------------------
    def _validate(self, contract: AgentContract) -> list[str]:
        problems: list[str] = []
        if not contract.agent_id:
            problems.append("agent_id is empty")
        if not contract.ddr_id:
            problems.append("ddr_id is empty")
        if not contract.version:
            problems.append("version is empty")

        unknown_caps = sorted(set(contract.capabilities) - KNOWN_CAPABILITIES)
        if unknown_caps:
            problems.append(f"unknown capabilities: {unknown_caps}")

        # S6 inverted: a contract must not be a self-signed grant of authority.
        for cap in contract.capabilities:
            if cap in KNOWN_CAPABILITIES and not self.lockbox.check(contract.ddr_id, cap):
                problems.append(f"{cap} claimed but not granted by the lockbox")

        unknown_inv = sorted(set(contract.invariants_pledged) - _VALID_INVARIANTS)
        if unknown_inv:
            problems.append(f"unknown invariants pledged: {unknown_inv}")
        return problems

    # -- registration --------------------------------------------------------
    def register(self, contract: AgentContract) -> AgentContract:
        self.lockbox.require(DDR_ID, CAPABILITY)
        problems = self._validate(contract)
        if problems:
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="contract.rejected",
                gate_status="rejected", agent_id=contract.agent_id,
                problems=problems,
            )
            raise ContractError(
                f"contract for {contract.agent_id!r} invalid: {'; '.join(problems)}"
            )
        self._contracts[contract.agent_id] = contract
        self._fingerprints[contract.agent_id] = contract.fingerprint()
        with self.store_path.open("a", encoding="utf-8") as fh:
            fh.write(contract.to_json() + "\n")
        self.audit.append_action(
            ddr=DDR_ID, file=_FILE, action="contract.register", gate_status="registered",
            agent_id=contract.agent_id, agent_ddr=contract.ddr_id,
            version=contract.version,
        )
        return contract

    def verify(self, agent_id: str) -> tuple[bool, list[str]]:
        """(valid, violations) for a registered agent."""
        contract = self._contracts.get(agent_id)
        if contract is None:
            return False, [f"no contract registered for {agent_id!r}"]
        violations = self._validate(contract)
        expected = self._fingerprints.get(agent_id)
        if expected is not None and contract.fingerprint() != expected:
            violations.append("contract fingerprint changed after registration")
        return (not violations), violations

    def _as_agent_contract(self, contract: Any) -> AgentContract:
        """Normalise a mesh PeerContract into an AgentContract for comparison.

        B-12 hands the verifier its own PeerContract, which carries no schemas.
        Borrowing the registered contract's schemas keeps the comparison to the
        fields the peer actually declared, instead of failing every peer for a
        difference it had no way to express.
        """
        if isinstance(contract, AgentContract):
            return contract
        registered = self._contracts.get(getattr(contract, "agent_id", ""))
        return AgentContract(
            agent_id=getattr(contract, "agent_id", ""),
            ddr_id=getattr(contract, "ddr_id", ""),
            version=getattr(contract, "version", ""),
            input_schema=dict(registered.input_schema) if registered else {},
            output_schema=dict(registered.output_schema) if registered else {},
            capabilities=tuple(getattr(contract, "capabilities", ())),
            invariants_pledged=tuple(getattr(contract, "invariants_pledged", ())),
        )

    def verify_contract(self, contract: Any) -> bool:
        """Verifier hook for B-12's mesh join — replaces reject_all_verifier."""
        presented = self._as_agent_contract(contract)
        problems = self._validate(presented)
        registered = self._contracts.get(presented.agent_id)
        if registered is not None and registered.fingerprint() != presented.fingerprint():
            problems.append("presented contract differs from the registered one")
        if problems:
            self.audit.append_action(
                ddr=DDR_ID, file=_FILE, action="contract.verify_failed",
                gate_status="rejected", agent_id=presented.agent_id, problems=problems,
            )
        return not problems

    def get(self, agent_id: str) -> AgentContract | None:
        return self._contracts.get(agent_id)

    def all(self) -> list[AgentContract]:
        return sorted(self._contracts.values(), key=lambda c: c.agent_id)

    def pledges(self, agent_id: str) -> tuple[str, ...]:
        """Invariants this agent promised — D-08 checks compliance against these."""
        contract = self._contracts.get(agent_id)
        return contract.invariants_pledged if contract else ()


def contract_for(
    agent_id: str,
    ddr_id: str,
    capability: str,
    lockbox: Lockbox,
    version: str = "1.0.0",
    invariants: tuple[str, ...] = (),
    input_schema: dict[str, Any] | None = None,
    output_schema: dict[str, Any] | None = None,
) -> AgentContract:
    """Build a contract carrying the agent's real lockbox token."""
    token = lockbox.get_token(ddr_id)
    return AgentContract(
        agent_id=agent_id,
        ddr_id=ddr_id,
        version=version,
        input_schema=dict(input_schema or {}),
        output_schema=dict(output_schema or {}),
        capabilities=(capability,),
        invariants_pledged=invariants,
        lockbox_token=token.token if token else "",
    )
