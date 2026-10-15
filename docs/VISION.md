# PRADYOS — Operator Vision

**Read this first.** One page. Set by the operator on PR #44 (comment
6177173413, OWNER), recorded by DDR-1169. Where any other document disagrees
with this page, this page wins and the other document is stale.

## 1. Agents operate the machine like a skilled human operator

Agents work with files, a shell, a browser, editors and build tools — the same
surfaces a person uses. They reach every one of them **only through
capability-checked syscalls**:

* **No ambient authority.** A process can do nothing it was not handed a
  capability for, kernel-side, at spawn.
* **No self-granting.** No agent can mint, widen or forge a capability for
  itself or for another agent. Capabilities are minted by the kernel and the
  sovereign daemon only.
* **Every action is audited** in the tamper-evident chain (F#76 tamper
  evidence, DDR-842; signed heads, DDR-1150).

The computer-use surfaces that make this real — structured UI-state reading,
input injection through the compositor, a sandboxed shell, a sandboxed browser
harness, per-task budgets and rollback — are scoped in **DDR-1168**. None of
them is built yet.

## 2. The operator approves anything that starts or rewrites work

Every **project start** and every **code rewrite** stays forced `PENDING` until
the operator approves it. Sovereign mode does not auto-approve these.

* Today `aether_action_forces_pending()` (`kernel/aether/aether.h`) forces
  `ACTION_SPAWN_PROCESS`, `ACTION_DELETE_FILE`, `ACTION_REWRITE_AGENT_CODE` and
  `ACTION_EVOLVE_GENOME`. That list is the floor; it may only grow.
* **Rejection stops the work and rolls it back.** Today a rejected or pending
  action is simply not executed (the agent obeys the verdict, DDR-1083/1084).
  Multi-step work that changed state before a later step was rejected has
  **no rollback yet** — that is DDR-1168 scope, not a shipped property.

## 3. Inference is local first

* **Local Ollama first.** The only inference path in the tree is
  `ollama_generate()` (`user/agent_base.c`). CI runs it in test mode.
* **Cloud is an explicit per-agent opt-in** and stays **deferred until the
  operator authorizes it** (DDR-793). Nothing enables it by default, and no
  session may enable it on its own judgment.

## 4. No "ASI" label

Nothing in the UI or the docs is called "ASI" or any equivalent
superlative. Each agent is documented by **what it measurably does** — the
action types it may submit, the capabilities it holds, and the gate that
proves it.

## 5. Quantum stays withdrawn

Quantum-hardware integration is **WITHDRAWN** (operator decision, PR #17;
`CLAUDE.md` §PHASE 3). Do not reopen it. Post-quantum *cryptography* (ML-DSA)
is a different thing and is shipped.

## 6. Superseded documents

`PRADYOS_Claude_Instructions.md` is the original build brief. Several of its
sections no longer describe this repository; it carries a banner and inline
annotations saying which (DDR-1169 §3). Where it disagrees with `CLAUDE.md`
or this page, it is wrong.
