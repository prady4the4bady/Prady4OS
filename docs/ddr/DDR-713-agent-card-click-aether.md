# DDR-713 — Agent-card click → trigger via AETHER (Layer 7)

> DDR before code, per the brief. DDR-707 renders the 8 named agents
> (KRYOS…SOLIN) as cards whose status dot reflects AETHER's roster, but the cards
> are inert. This slice makes them **interactive**: clicking an agent card triggers
> that agent through AETHER (`SYS_SPAWN_AGENT`), lighting its roster slot. It wires
> the desktop's pointer to the agent runtime — the first UI→agent action.

## Decisions

### D1 — The sovereign compositor triggers the agent
The compositor already runs with `CAP_SOVEREIGN` (`is_sovereign`), and
`SYS_SPAWN_AGENT` (35) permits a sovereign caller (`kernel/syscall/sys_aether.c`).
So no new syscall: on an agent-card click the compositor calls
`SYS_SPAWN_AGENT(_, name, slot)` — the kernel spawn hook loads the embedded agent
ELF as a fresh `CAP_AGENT` process and, because `slot` (a3) is a valid roster
index, sets `g_roster[slot] = 1` (DDR-707). The card's status dot turns green on
the next render. Submitting/approving actions stays the agent's/daemon's job
(authority is the kernel flag, never the compositor) — the compositor only
*launches* the agent, which is exactly "clicking an agent triggers it."

### D2 — Card hit-testing in the compositor pointer path
A new `agent_card_hit(x, y)` mirrors `render_agent_panel`'s layout (cards at
`x = width-210`, `y = 70 + i*44`, `200×36`) and returns the slot index under the
pointer, or −1. In the pointer **button-down** handler it is checked **first**
(the panel is chrome on the right edge; a card hit short-circuits the title-bar
drag / plain-click paths). On a hit the compositor calls `SYS_SPAWN_AGENT`, prints
`PRADYOS_AGENT_TRIGGER name=<NAME> slot=<i> pid=<pid>`, and re-renders so the slot
lights. The roster-change detector (already in the main loop) then prints
`AGENT <NAME> active` + `PRADYOS_AGENTS_OK`. No mouse device → `SYS_MOUSE_POLL`
returns `-ENODEV` and the whole path is skipped (non-tablet gates unaffected).

### D3 — Click a currently-inactive slot in the test
The daemon lights slot 0 (KRYOS) at boot, so slots 1–7 start inactive. The gate
clicks **card 1 (PRAX)** so the trigger produces a visible inactive→active
transition. `mouse_inject.sh` gains optional `ABSX`/`ABSY` env (defaults
16000/12000 unchanged, so `smoke-mouse` is untouched); the new gate sets them to
card 1's centre (≈ `29250,5632` in the tablet's 0..32767 abs space for 1024×768).

### D4 — Root-cause fix folded in: hook registration vs live scheduler (boot race)
The first gate runs exposed a latent boot race: `SYS_SPAWN_AGENT` returned
`-ENOSYS` for the compositor because `aether_set_spawn_hook()` was called in kmain
**after** all the user ELFs were loaded — but the preemptive scheduler runs
spawned threads *while kmain is still booting* (the remaining ELF loads +
self-tests take seconds of virtio I/O). The compositor rendered, printed
`PRADYOS_AGENTS_OK`, and took the harness click before kmain reached the
registration line; the daemon only spawns after kmain finishes, which is why the
race never fired before. (Diagnosed conclusively by QEMU-monitor `xp` of the
hook's physical address over time: 0 until kmain's late registration, valid
after — page tables and memory were always correct.) Fix: register the hook
**before the first `user_boot_from_sfs`**, so no user thread can ever observe it
unset. Agents spawned before the daemon exists parent to 0 (reaper collects);
`g_aether_daemon_pid` is filled in when the daemon loads, as before.

## Gate
`smoke-agent-click` (CI, GPU + virtio-tablet, QMP `input-send-event`): boot, wait
for `PRADYOS_AGENTS_OK` (panel rendered, KRYOS lit), inject a left click on card 1,
and grep `PRADYOS_AGENT_TRIGGER name=PRAX slot=1` + `AGENT PRAX active`. 45 CI
gates total. `smoke-agents` (roster render) and `smoke-mouse` (plain click, default
coords) keep passing.

## Non-goals (later)
Per-agent task strings / a launcher prompt; click-to-**kill** (`SYS_KILL_AGENT`) or
toggle; the approval-queue card UI (APPROVE/REJECT buttons, brief §; that is the
sovereign-mode panel, a later slice); hover/press spring animation on the cards;
routing a click to a specific agent *capability*. Deferred — this slice is
click-a-card → spawn-the-agent.
