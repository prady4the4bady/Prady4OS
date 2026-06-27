# ADR-026: Layer 6 — AETHER (AI-native agent layer)

- **Status:** Accepted 2026-06-28 — design record for Layer 6 (AETHER).
- **Date:** 2026-06-28
- **Phase:** Layer 6 (AETHER); follows NET-B (lwIP) and Layer 5 (userspace).
- **Relation to prior ADRs:** builds on **ADR-022** (NSI syscall expansion),
  **ADR-009** (NCS capabilities), **ADR-008/016** (scheduler/preemption),
  **ADR-023** (musl userspace), **ADR-025** (lwIP — the agent's only network
  path). Bound by **ADR-021** (W^X). Supersedes nothing.

> **Why an ADR?** AETHER lets a (potentially model-driven, potentially hostile)
> ring-3 agent ask the kernel to take consequential actions. The trust boundary,
> the human-in-the-loop model, the resource caps, and the audit trail must be
> decided **before** code so safety is structural, not bolted on. A binding
> invariant here (the sovereign approval gate) may only be changed by a new ADR.

---

## Decisions

### D1 — Trust model: the kernel is the arbiter, the agent is untrusted
An *agent* is an ordinary ring-3 process with **no special privilege**. It cannot
touch hardware, memory, or the filesystem except through the NSI, exactly like
any other process. What makes it an "agent" is only that it speaks the AETHER
syscalls. Every consequential action it wants to take is **proposed** to the
kernel as data and is only carried out after an explicit policy check. The agent
never holds the authority to act; the kernel does. This keeps a compromised or
mis-aligned model strictly inside the existing isolation guarantees.

### D2 — Two modes: sovereign (auto) and manual (human-in-the-loop)
A single global `g_sovereign_mode` (kernel BSS, `u32`, **default 1 = sovereign**)
selects the approval policy:
- **Sovereign (1):** `SYS_SUBMIT_ACTION` auto-approves the action immediately and
  records it; the agent's `SYS_POLL_RESULT` sees `APPROVED` on the next poll.
- **Manual (0):** a submitted action sits `PENDING` until a human (via the daemon's
  IPC surface → `SYS_APPROVE_ACTION` / `SYS_REJECT_ACTION`) decides, or it
  **expires** after 60 s.

Mode is read with `SYS_GET_MODE` (unprivileged) and changed with `SYS_SET_MODE`,
which **requires `CAP_SOVEREIGN`** (see D6). The default is sovereign so the
reference build is autonomous out of the box; a deployment that wants a human
gate flips one flag. The choice of default is deliberately recorded here so it
can only be changed by a superseding ADR.

### D3 — Action queue (kernel-owned, bounded)
A fixed **256-entry circular action queue** lives in kernel BSS (no heap, no
per-action allocation — bounded memory under flood). Each entry:

```
{ action_id u64, agent_pid u32, action_type u32, payload[512], status }
status ∈ { FREE, PENDING, APPROVED, REJECTED, EXPIRED, DONE }
```

`action_id` is a monotonically increasing kernel counter (never reused within a
boot) so an agent cannot forge or alias another agent's action. `payload` is a
fixed 512-byte inline buffer copied in from user space via the validated
`copyin` path (ADR-022) — never a user pointer the kernel later dereferences.
Submitting when the queue is full returns **`-EAGAIN`** (no overwrite, no crash).
Expiry (60 s) is evaluated lazily on `SYS_POLL_RESULT`/submit using the PIT tick
clock (`g_ticks`, 100 Hz) — no timer callback needed.

**Ownership:** `SYS_POLL_RESULT`, `SYS_APPROVE_ACTION`, `SYS_REJECT_ACTION` only
operate on entries whose `agent_pid`/authority matches the caller's rights:
an agent may poll only its **own** actions; approve/reject require `CAP_SOVEREIGN`
(the human operator's authority, held by the daemon), so an agent can never
approve itself even in manual mode.

### D4 — Audit log (append-only, circular, wrap-flagged)
A **4096-entry circular audit log** in kernel BSS records every decision:

```
{ timestamp u64, agent_pid u32, action_id u64, action_type u32, result u32 }
```

`timestamp` is `g_ticks` (monotonic since boot). The log is **append-only** from
user space — there is no erase/rewrite syscall. When the ring wraps, a single
`AETHER_AUDIT_WRAP` event is emitted to the serial console so the loss of the
oldest records is itself auditable. `SYS_READ_AUDIT` copies entries out to a user
buffer via `copyout` (validated). The log captures submit, approve, reject,
expire, OOM-kill, rate-limit-kill, and cap-escalation-denied events.

### D5 — Memory cap + OOM kill (per process)
Each process carries a **hard memory cap** (`mem_limit`, default **128 MiB**) and
a running `mem_used` counter, both in the TCB (appended at struct end to preserve
the ~30 includers' field offsets). The cap is charged at the two places a process
grows its address space: `SYS_MMAP` (anonymous arena) and `SYS_BRK`-equivalent
growth. A request that would push `mem_used` past `mem_limit` does **not** get the
memory; the agent is **cleanly killed** (`sched_exit`-style teardown, never a
kernel panic) and `AGENT_OOM_KILLED PID=N` is logged to serial + audit.
`SYS_SET_MEM_LIMIT` may only **lower** a process's own cap or set a child's cap;
it can never raise its own above the inherited ceiling (no self-escalation).

### D6 — Capability model (NCS bits)
Two new NCS rights bits (ADR-009 `cap.h`), appended without disturbing existing
bits:
- **`CAP_SOVEREIGN` (1<<16):** authority to change the global mode
  (`SYS_SET_MODE`) and to approve/reject queued actions. Held by the operator /
  the AETHER daemon, **never minted into an agent's cap table.**
- **`CAP_AGENT` (1<<17):** marks a process as an agent permitted to use
  `SYS_SUBMIT_ACTION`/`SYS_POLL_RESULT`/`SYS_SPAWN_AGENT`. Granted by the daemon
  when it spawns an agent.

A syscall that needs a right and does not find it returns **`-EPERM`** and writes
a `cap-escalation-denied` audit entry; the caller **survives** (a denied
escalation is not fatal — only resource abuse is). `cap_delegate`/`cap_restrict`
already guarantee rights can only ever be subset, so an agent cannot widen its
own authority (ADR-009 no-confused-deputy holds verbatim).

### D7 — Rate limiting (syscall sliding window)
The syscall dispatcher enforces a **60 syscalls / 1 s sliding window per
process** for agent processes. The window is two TCB fields (`sc_window_start`
tick, `sc_count`); when `g_ticks` advances past the 1 s (100-tick) boundary the
window resets. Exceeding the budget **cleanly kills** the offending process and
logs `AGENT_RATE_LIMITED PID=N`. Non-agent processes (init, PRISM) are exempt so
the existing gates are unaffected. This bounds a tight-loop agent's ability to
DoS the single core.

### D8 — `SYS_SPAWN_AGENT` always requires approval for process spawn
Spawning a *new process* is the one action that is **never** auto-approved, even
in sovereign mode (`ACTION_SPAWN_PROCESS` is force-routed to `PENDING`). Spawning
compute that can spawn more compute is the highest-consequence action an agent
can request, so it always needs an explicit human/operator decision. `SYS_KILL_AGENT`
(terminate an agent the caller spawned) is allowed under `CAP_AGENT`.

### D9 — Model runner integration (Ollama, ring-3 only, via lwIP)
Live model calls are **HTTP/1.1 from ring 3** to an Ollama endpoint
(`/api/generate`) over the NET-B TCP stack — the kernel never speaks HTTP and
never embeds a model. The agent template (`user/agent_base.c`) opens the socket,
POSTs the prompt, and parses the `response` field with a **minimal hand-written
JSON scanner** (no external lib). The kernel's only network role remains lwIP
packet plumbing. No kernel address, pointer, or ring-0 datum is ever placed in a
prompt, an HTTP body, or an agent's output (W^X + info-leak hygiene per ADR-021).

### D10 — Test mode (CI has no live model)
`AETHER_TEST_MODE` (delivered via kernel cmdline → daemon config) makes the agent
template **skip the network entirely** and use a fixed response:
`ACTION: WRITE_FILE /tmp/aether_test.txt PRADYOS_AGENT_VERIFIED`. This lets the
full submit→approve→execute→audit pipeline run deterministically in CI with no
external dependency. Live mode is identical except the response comes from Ollama.

### D11 — Process topology
`pradyos-init` (PID 1) spawns the **AETHER daemon** (`user/aether_daemon.c`) as a
long-lived process after PRISM. The daemon holds `CAP_SOVEREIGN`, reads
`/etc/aether/config` from SFS, exposes the NIA IPC command surface
(`AETHER_SPAWN/STATUS/KILL/MODE`), and in test mode auto-spawns one agent. Agents
(`user/agent_base.c`) hold `CAP_AGENT` only. The daemon prints
`PRADYOS_AETHER_DAEMON_OK`; an agent prints `PRADYOS_AGENT_DONE` on clean exit.

---

## Security model (summary of invariants)

1. **No self-escalation** — an agent cannot raise its own cap rights or mem cap,
   nor approve its own actions (D3/D5/D6).
2. **Bounded everything** — queue (256), audit (4096), payload (512 B), memory
   (128 MiB), syscalls (60/s). Every bound returns an error or a clean kill,
   never a panic (D3/D4/D5/D7).
3. **Human gate is structural** — sovereign default is recorded here and the
   approval authority lives behind `CAP_SOVEREIGN`; process spawn always needs
   approval (D2/D8).
4. **Append-only audit** — no user path erases history; wraps are flagged (D4).
5. **Fault isolation** — a ring-3 agent fault kills the agent, never the kernel;
   all user pointers cross the `copyin`/`copyout` boundary (ADR-022).
6. **No info leak** — no kernel address appears in any agent-visible output,
   network body, or audit field exposed to ring 3 (D9).

## Gates (CI)
- **`smoke-aether-queue`** — kernel submits a test action, sovereign auto-approve,
  audit entry written → `PRADYOS_AETHER_QUEUE_OK`.
- **`smoke-aether`** — daemon init (`PRADYOS_AETHER_DAEMON_OK`) → auto-spawn test
  agent → `ACTION_WRITE_FILE` → approved → file written → `PRADYOS_AGENT_DONE`.
- **`smoke-aether-sec`** — OOM kill, cap-escalation denied (survives), rate-limit
  kill, queue overflow `-EAGAIN`, audit wrap — all expected outcomes asserted.

## Alternatives considered
- **Heap-allocated queue/audit** — rejected: unbounded under flood; fixed BSS
  rings are simpler to reason about and DoS-resistant.
- **Per-action user pointer kept in the queue** — rejected: TOCTOU + info-leak
  risk; we copy a fixed payload in once (D3).
- **Agent-side approval** — rejected: defeats the human gate; approval authority
  is kernel-side behind `CAP_SOVEREIGN` (D6/D8).
- **In-kernel model runner** — rejected: enormous attack surface in ring 0 and a
  W^X hazard; inference stays in ring 3 over TCP (D9).
