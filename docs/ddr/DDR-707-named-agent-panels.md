# DDR-707 — Named-agent UI panels (Layer 7)

> DDR before code, per the brief. Renders the 8 named agents (KRYOS, PRAX, LUMYN,
> AHNIS, IRIS, RUFLO, HERMES, SOLIN) as cards in the compositor, each showing an
> active/inactive state tied to AETHER's agent registry — matching the design
> images' agent panel. Builds on the framebuffer + compositor (DDR-704/706).

## Assessment — is a new syscall needed? (yes, one read-only)

The AETHER layer (NSI 29–38) tracks generic `CAP_AGENT` processes + the audit
ring (`SYS_READ_AUDIT`) — it has **no per-name agent registry**, and the audit ring
returns activity entries, not an 8-slot roster. Showing 8 named cards with per-slot
active state therefore needs the kernel to expose a small roster. Decision: add an
**8-slot active-bit roster** to the AETHER layer and **one** new read-only syscall
to query it. Spawning is already a syscall (`SYS_SPAWN_AGENT`); we reuse it to
mark a slot active (no new write syscall).

## Decisions

### D1 — AETHER 8-slot agent roster (kernel)
`kernel/syscall/sys_aether.c` gains `static uint8_t g_roster[8]` (BSS): one
active bit per roster slot. `SYS_SPAWN_AGENT(path, task, slot)` — the previously
unused 3rd arg is now the **roster slot**; on a successful spawn the kernel sets
`g_roster[slot]=1` (`0 ≤ slot < 8`). The bit means "this named agent has been
activated this session" — real AETHER state, **stable** for the panel (it does not
flicker with the short-lived test agent). An explicit clear-on-kill (a pid→slot
map) is deferred — agents are session-scoped here.

### D2 — One new read-only syscall
```
SYS_AGENT_ROSTER(u8 __user *buf, int max) -> count   [53]
```
Copies out up to `max` (≤8) roster active-bytes. Read-only and open (the panel
reads display state only, no authority). No write syscall — activation flows
through the existing `SYS_SPAWN_AGENT`.

### D3 — Names live in userspace; kernel tracks bits by index
The 8 names are UI constants in the compositor (and the daemon picks the slot it
spawns into); the kernel only tracks 8 bits by index. The AETHER daemon spawns the
test agent into **slot 0 (KRYOS)**, so KRYOS shows active and the rest inactive —
honestly reflecting that one agent ran. (A fuller roster lights up as more named
agents are spawned; the panel is data-driven.)

### D4 — Compositor agent panel
The compositor (DDR-704) gains a right-side **agent panel**: 8 cards, each a small
glass-ish rectangle with the agent name (8×8 font) and a status dot — **green** if
`g_roster[i]` is active, **dim** otherwise — read via `SYS_AGENT_ROSTER`. It
re-renders the panel when the roster changes and, the first time it reads the
roster (and on each change), prints the roster to serial:
`AGENT <NAME> active|inactive` ×8 then `PRADYOS_AGENTS_OK`. Event-driven (only on
roster change) — no continuous flushing.

## Gate

`smoke-agents` (CI, `QEMU_GPU=1`): boot; the compositor renders the agent panel and
prints all 8 named agents with state, then `PRADYOS_AGENTS_OK`; the daemon's spawn
lights KRYOS (`AGENT KRYOS active`). The gate greps `PRADYOS_AGENTS_OK` + the 8
names + `AGENT KRYOS active` (state tied to the real spawn).

## Non-goals (later)
Per-agent live metrics (CPU/RAM/task), the "8 Active" badge animation, click-to-
focus an agent panel, agent spawn/kill from the panel UI, and the full glass/OKLab
card styling — deferred. This is the named roster + live active/inactive state.
