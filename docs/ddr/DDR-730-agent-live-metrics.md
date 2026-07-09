# DDR-730 — Per-agent live metrics + roster liveness root-fix

**Status:** proposed (pre-code)
**Layer:** 7 (desktop) / cross-cut with AETHER (Layer 6) + proc
**Extends:** DDR-707 (named-agent roster), ADR-026 / DDR-AETHER.

## Problem

The agent panel (DDR-707) shows the 8 named agents (KRYOS..SOLIN) as cards, each
with a green dot when `g_roster[slot]` is set. Two shortcomings:

1. **The active bit leaks (latent bug).** `g_roster[slot]` is set at
   `SYS_SPAWN_AGENT` and **never cleared** — `sys_kill_agent` only posts SIGKILL,
   normal agent exit clears nothing, and `aether_drop_pid` (called on OOM/rate
   kill) touches the queue, not the roster. DDR-707's comment claims the bit is
   "cleared when that agent is killed", but nothing does it. A card stays green
   forever after its agent dies.

2. **No live metrics.** The dot is binary. The kernel already tracks real
   per-agent state on the tcb — `state` (running/blocked), `mem_used`
   (aether_mem charge), and could cheaply count submitted actions — but none of
   it is exposed, so the panel can't show what an agent is *doing*.

## Decision — liveness is derived, metrics come from the live tcb

**Enrich the roster, derive `active` lazily.** Replace `uint8_t g_roster[8]`
with `struct agent_slot { uint8_t used; uint32_t pid; uint64_t actions; }
g_agent[8]`. `SYS_SPAWN_AGENT` records `used=1, pid, actions=0`. A slot is
reported **active iff `used && tcb_by_pid(pid)` resolves to a live (non-zombie)
agent tcb.** This makes the active bit self-correcting: when the agent dies its
tcb goes zombie/gone, `tcb_by_pid` returns NULL, and the card goes dim on the
next query — **no teardown hook needed**, which root-fixes the DDR-707 leak. (A
recycled pid cannot false-positive: pids are monotonic, `next_tid++`.)

**Metrics read straight from the live tcb** at query time:
- `state`: 0 = inactive/dead, 1 = ready/running, 2 = blocked (from `tcb.state`).
- `mem_used`: the agent's charged bytes (existing `tcb.mem_used`).
- `actions`: cumulative actions the agent has submitted — a new per-slot counter
  bumped in `sys_submit_action` (find the submitter's slot by
  `current_thread->pid`). Cheap, monotonic, genuinely "live".

**New syscall `SYS_AGENT_METRICS` (NSI 64).** `MAX_SYSCALLS` is bumped from 64 to
80 (round headroom; the table is 8-byte fn pointers — negligible BSS). Returns up
to 8 × `struct agent_metric { uint32_t pid, state; uint64_t mem_used, actions; }`
via `copyout` (staged in a bounded kernel buffer; never a raw user deref,
ADR-022). Inactive slots report all-zero. No authority gate — the metrics are
read-only observability the compositor (or any UI) may read, exposing nothing an
agent could forge or abuse.

**`SYS_AGENT_ROSTER` (NSI 53) is preserved** — reimplemented to emit its active
bits from the same lazy-liveness check, so the existing compositor path and
`smoke-agents` keep working *and* inherit the leak-fix (a card now dims when its
agent exits).

**Compositor.** `render_agent_panel` reads `SYS_AGENT_METRICS`; each active card
draws its live action count beside the status dot. The serial roster report
(already emitted on roster change) gains a per-agent metric line so the change is
observable off the framebuffer.

## Gate — `smoke-agentmetrics` (76 gates)

Reuses the existing agent bring-up (the daemon spawns the test agent into slot 0
= KRYOS, which submits + completes an action, then exits). On `QEMU_GPU=1` (the
compositor renders). The compositor, when it first sees KRYOS active, reads
`SYS_AGENT_METRICS` and prints:

- `AGENT_METRIC KRYOS pid=<n> st=<s> act=<a> mem=<m>` — with `st >= 1` (the agent
  is provably *alive*, not a stuck bit) for the active agent, and
- `PRADYOS_AGENT_METRICS_OK`.

The gate asserts `PRADYOS_AGENT_METRICS_OK` and a KRYOS metric line with a live
state. Forbidden: `AGENT_METRICS FAIL`. Because liveness is now derived, the same
run also demonstrates the leak-fix: after the test agent exits, a later roster
report shows `AGENT KRYOS inactive` — the card correctly dims. `smoke-agents`
stays green (it only requires that KRYOS is seen active at least once, which
still holds during the agent's life).

Regression: `smoke-agents`, `smoke-agent-click`, `smoke-aether`,
`smoke-aether-sec`, `smoke-mode`, `smoke-compositor`, then the full suite.

## Non-goals

- No CPU-time metric — the tcb has no per-thread run-tick accounting today, and
  adding one is a scheduler change out of scope here. `state` + `actions` +
  `mem_used` are the live signals this slice surfaces.
- No change to agent authority or the action-arbitration flow.
- wlroots/Wayland remain out-of-tree.
