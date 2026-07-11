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

**Compositor (leak-fix only, no growth).** The panel already reads
`SYS_AGENT_ROSTER`; because that call now returns lazy-liveness bits, the status
dot **dims automatically when an agent dies** with no compositor change — the
DDR-707 leak is fixed for free. *Implementation note:* an earlier plan had the
compositor render numeric metrics + activity pips, but the added code pushed the
musl-linked compositor ELF across a 4 KiB segment boundary and blew the kernel
image budget. The metrics witness therefore lives in a dedicated freestanding
probe (below), keeping the compositor untouched.

**Kernel image budget (load raised 512 -> 544 KiB — the physical ceiling).** The
512 KiB stage-2 load window (`boot/stage2/stage2.asm`, 16 chunks) was exhausted —
the surface-destroy slice consumed the last freestanding-ELF headroom, so even a
~5 KiB probe overran it. First attempt raised the load to 24 chunks (768 KiB);
the boot gates immediately caught that as **physically impossible**: conventional
RAM ends at 0x9FC00 (E820), so a real-mode flat load at 0x10000 tops out at
~575 KiB — chunks past 17 write into the VGA/ROM hole and the loader hangs.
Corrected to 17 chunks (544 KiB, ends 0x98000 < 0x9FC00), which covers this
slice's 529 KiB kernel with headroom. The Makefile size-check now states the real
constraint: growth past 544 KiB requires relocating the kernel above 1 MiB
(unreal-mode bounce copy or a PM disk driver) — a dedicated boot slice, not a
chunk-count bump.

## Gate — `smoke-agentmetrics` (76 gates)

Reuses the existing agent bring-up (the daemon spawns the test agent into slot 0
= KRYOS in test mode). A dedicated freestanding probe `user/agentmetricstest.c`
(musl-free, `user.ld`, no writable globals — like `surfdestroytest`) polls
`SYS_AGENT_METRICS` and, when it observes slot 0 (KRYOS) live (`state >= 1 &&
pid != 0`) *while* an unspawned slot (7 = SOLIN) reads idle (`state == 0`), prints:

- `AGENT_METRIC KRYOS live pid ok` — the metric reflects a genuinely-running pid,
  discriminating live from idle (not a stuck bit, not a blanket all-active), and
- `PRADYOS_AGENT_METRICS_OK`.

The gate asserts both lines; forbidden `AGENT_METRICS FAIL` (the probe prints it
and exits non-zero if it never observes a live agent). No GPU needed — the probe,
not the compositor, is the witness. `smoke-agents` stays green (KRYOS is still
seen active during the agent's life; it now also correctly dims afterward).

**`hello.asm` per-char output root-fixed (flake caught in regression):**
`smoke-smpuser` failed ~50% — `HELLO FROM RING-3` shredded mid-line into a BSP
`kputs`. `user/hello.asm` (Phase 5a) printed per-char via `SYS_PUTC`: each char
is individually locked, but chars from an AP freely interleave with other CPUs'
lines — inherent to a per-char API under SMP, and this slice's timing shifts
made it reproducible. Fixed at the source: the line is now ONE `SYS_WRITE`
(atomic `kwrite` unit per the rq-3 contract); the trailing newline stays on
`SYS_PUTC` so NSI 1 keeps coverage. 6/6 deterministic after.

**`smoke-agent-click` updated (regression caught in testing):** its old
`AGENT PRAX active` assertion depended on the sticky bit — with live-derived
liveness, a clicked test agent runs and exits within milliseconds, so the
compositor's sampled roster report may never catch it alive (a race, and
asserting it would make the gate flaky). The deterministic witness for DDR-713's
contract is the chain `PRADYOS_AGENT_TRIGGER name=PRAX slot=1` followed by a
`PRADYOS_AGENT_DONE` — the click provably spawned an agent that ran to
completion. The transient card-lighting remains a live-view property, covered
for a long-lived agent by `smoke-agents` (KRYOS).

Regression: `smoke-agents`, `smoke-agent-click`, `smoke-aether`,
`smoke-aether-sec`, `smoke-mode`, `smoke-compositor`, then the full suite.

## Non-goals

- No CPU-time metric — the tcb has no per-thread run-tick accounting today, and
  adding one is a scheduler change out of scope here. `state` + `actions` +
  `mem_used` are the live signals this slice surfaces.
- No change to agent authority or the action-arbitration flow.
- wlroots/Wayland remain out-of-tree.
