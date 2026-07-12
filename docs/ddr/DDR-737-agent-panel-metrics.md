# DDR-737 — Agent-panel live metrics (hardening 3/3)

**Status:** proposed (pre-code)
**Layer:** 7 (compositor UI) — live-agent hardening campaign, final slice.
**Extends:** DDR-707 (agent cards), DDR-730/735 (SYS_AGENT_METRICS).

## Problem

The agent panel shows only the liveness dot. The kernel now exposes real
per-agent metrics — `state`, `actions`, `run_ticks`, `dispatches`, retained
post-mortem (DDR-735) — but nothing renders them, so the operator still can't
tell a busy agent from a blocked one, or see that a completed agent did work.
This was DDR-730's original plan, reverted then because the compositor's +4 KiB
would have crossed the 544 KiB image ceiling; DDR-733's 768 KiB window leaves
~240 KiB of headroom, so the constraint is gone.

## Decision

`render_agent_panel` reads `SYS_AGENT_METRICS` (the roster read goes away —
metrics subsume it: `state >= 1` is the DDR-730 lazy-liveness bit):

- **Status dot, state-colored:** green = running/ready (`state 1`), amber =
  blocked (`state 2`), gray = dead/empty (`state 0`). A slot with retained work
  (`pid != 0`, state 0) draws a dim green dot — "ran, now done" — so a completed
  agent is distinguishable from a never-spawned one.
- **Activity pips:** up to 4 small squares per card, one per submitted action
  (`min(actions, 4)`) — font-free, matching the card's glass style.
- **Serial witness (the gate's hook):** when the panel first observes slot 0
  with `pid != 0 && dispatches >= 1` (post-mortem stable, per DDR-735), it
  prints `AGENT_PANEL KRYOS act=<n> disp=<n>` and
  `PRADYOS_AGENT_PANEL_METRICS_OK` once. Counts are printed as decimals via the
  existing printf path (the compositor is musl-linked).

## Gate — `smoke-agentpanel` (80 gates)

`QEMU_GPU=1` (the compositor renders); asserts
`PRADYOS_AGENT_PANEL_METRICS_OK` + an `AGENT_PANEL KRYOS` line. Deterministic by
the DDR-735 argument: the facts are retained after the agent exits, so the
compositor's sampling cadence (seconds-long frames on TCG) cannot miss them.
Timeout 150 s like the other agent gates (late daemon spawn under CI load).

Regression: `smoke-agents` (the roster-change serial report must keep working),
`smoke-agentmetrics`, `smoke-compositor`, the visual set, then the full suite.

## Non-goals

- No numeric text rendering on cards (the 8×8 font has no digits embedded;
  adding glyphs is cosmetic scope — pips + dot state carry the signal).
- No per-agent history/graphs; no compositor polling-rate change.
