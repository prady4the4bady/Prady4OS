= DDR-914 — the serial roster witness must read the post-mortem-stable plane

**Status:** ACCEPTED — governs the `smoke-agents` fix.
**Date:** 2026-08-12
**Lineage:** DDR-707 (named-agent panel) → DDR-730 (liveness is not a stored bit)
→ DDR-735 (post-mortem-stable metrics) → **DDR-914 (this)**.

## The defect

`smoke-agents` fails with the roster reporting `AGENT KRYOS inactive` while
`AGENT_PANEL KRYOS act=1 disp=1` is emitted in the same boot. Both statements
are true of their own data plane; the gate's expectation is what is wrong.

This is a **reporting/sampling defect, not a liveness defect.** KRYOS is
genuinely spawned and provably scheduled — `disp=1` is the kernel's authoritative
switch-in count, captured at exit by `agent_metrics_reap`.

## Root cause — established by code read, no instrumentation required

Two independent confirmations, both from source:

1. **`kernel/syscall/sys_aether.c:52` — `roster_active()` is pure instantaneous
   liveness.** A slot reads 1 **iff** its pid still resolves to a live, non-
   `THREAD_ZOMBIE`/non-`THREAD_DONE` tcb. This is deliberate and load-bearing:
   DDR-730 specifies that a card *self-corrects to inactive* when its agent dies.
   It is correctly NOT sticky.

2. **`user/compositor.c:870-882` — the serial report only fires on an observed
   change** in that instantaneous sample. The report is therefore only emitted if
   a compositor frame lands *inside* the agent's live window.

On a TCG (KVM-less) CI runner a compositor quantum can take seconds, and the
agent's entire 0→1→0 lifecycle fits between two consecutive polls. The
transition is never sampled, `changed` never observes the 1, and the roster
reports `inactive` for the whole boot.

**This is the same race class DDR-735 already fixed one path over.** The
identical failure mode is documented verbatim at `user/compositor.c:883-886`
("the agent's whole life fits inside one slow compositor frame on TCG runners")
and at `Makefile:2592-2597` for `smoke-agentmetrics`. The metrics path was moved
onto a post-mortem-stable fact and stopped flaking; the roster path was left on
the instantaneous plane and still flakes. It is not a new problem.

## Decision

**Do NOT make the roster sticky.** Making `roster_active()` latch would silently
supersede DDR-730's self-correcting card semantics, which is binding UI
behaviour and may only be changed by a superseding DDR — never quietly amended.
A sticky roster would also make a dead agent's card render as alive, which is a
real UI lie, not merely a test convenience.

Instead, **separate the two questions the one print was conflating:**

| question | plane | consumer |
|---|---|---|
| "is this agent alive *right now*?" | `SYS_AGENT_ROSTER` (instantaneous) | the rendered card |
| "does this slot hold an agent?" | `SYS_AGENT_METRICS` (`pid != 0`, retained) | the serial witness |

Concretely, in `user/compositor.c`:

- The **card render** stays driven by the live `SYS_AGENT_ROSTER` sample. UI
  truth is unchanged; DDR-730 is untouched.
- The **serial roster report** becomes **one-shot**, keyed on the same
  post-mortem-stable trigger the panel witness already uses
  (`m[0].pid != 0 && m[0].dispatches >= 1`), with each slot's reported state
  derived from `m[i].pid != 0` — *spawned*, not *currently alive*.

`pid` is retained after exit by design (`sys_aether.c:241-243`: "a spawned
slot's IDENTITY" survives), so this is stable regardless of frame cadence.

## Why this is not weakening the gate

`AGENT KRYOS active` now asserts "slot 0 holds a spawned agent", gated behind
`dispatches >= 1` — i.e. it still cannot pass unless the agent was **provably
scheduled by the kernel**. A spawn that never ran does not satisfy the trigger
and the witness never prints. `AGENT SOLIN inactive` still requires slot 7 to
hold no agent at all. The gate proves strictly more than a lucky sample did.

## `PRADYOS_AGENTS_OK` must NOT move — second consumer found

`PRADYOS_AGENTS_OK` has a **second consumer that is not a gate assertion**:
`Makefile:1662`, where `smoke-agent-click` passes it to `mouse_inject.sh` as the
**readiness trigger** — the injector waits for that line before clicking agent
card 1.

Moving it onto the stable plane would have silently changed its meaning from
"the compositor's roster loop is live" (emitted almost immediately) to "an agent
has spawned *and* been dispatched" (which the daemon lands late — the reason
`smoke-agents` runs at `TIMEOUT_S=150`). `smoke-agent-click` is bounded at 120s,
so the click would have been pushed toward or past its own timeout: a passing
gate turned flaky by a fix aimed at a different gate.

**Therefore:** `PRADYOS_AGENTS_OK` stays exactly where it was, in the live
`changed` block, with its original readiness semantics. Only the eight per-slot
`AGENT <name> <state>` lines move to the post-mortem-stable one-shot. The two
sentinels are decoupled because they answer different questions for different
consumers.

This is the general rule restated: a sentinel's *timing* is part of its contract
whenever any consumer waits on it, not just greps for it.

## Sentinel boundary

**No sentinel strings change.** `AGENT KRYOS active`, `AGENT SOLIN inactive`,
and `PRADYOS_AGENTS_OK` keep their exact spellings, so `Makefile:2587` needs no
edit and no producer/consumer pair moves. `sentinel_collision.sh` is run anyway
to confirm, per the standing rule.

## Not to be done

- Raising `TIMEOUT_S` on `smoke-agents`. It is already 150s and the failure is
  not slowness — the transition is *missed*, not *late*, so no timeout makes it
  observable.
- Polling the roster faster. That shortens the race window without closing it,
  and would be an unmeasured constant.
