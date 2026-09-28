# DDR-1022 — Group F (F#66–F#76) assessed against the tree

**Status:** ASSESSMENT. **Two items are already shipped and gated and the tracker
says they are not.** Of the nine genuinely unbuilt, one is blocked by a decision
already taken, and eight need domain behaviour that does not exist. No code
change; the deferrals are logged so §WHAT "DONE" MEANS's *"zero unlogged
exclusions"* can hold for Group F.

---

## 1. Method, and why it is the finding

DDR-1020 §1 records two of my own DDRs that declared a 3C type unbuilt **from
memory** and were wrong. DDR-1021 fixed the method: grep first, and let the grep
be the answer. Applied to Group F, the grep immediately contradicted the tracker
**twice**. That is now four instances of the same error class in this repo, so
the method matters more than any single row below.

## 2. The structural fact that reframes "11 unbuilt agents"

**There is exactly ONE agent program in the tree:** `user/agent_base.c`. The
named agents are not separate binaries — the roster is a generic array of
active-bits (`sys_agent_roster`, `AGENT_ROSTER_N`), and a "slot" is filled by
`SYS_SPAWN_AGENT` (NSI 35) launching `agent_base` with a task string. The kernel
holds **no per-agent identity at all**: `grep` for KRYOS/PRAX/RUFLO and friends in
`kernel/aether/*.c` returns nothing.

So "build 11 agents" is not 11 programs — it is **11 domain behaviours**, each of
which needs a real observable effect or its gate asserts a sentinel and nothing
else. This session hit that vacuity trap five separate times (DDR-1016 §5,
DDR-1017 §4, DDR-1018 §3, DDR-1020 §5 twice). Shipping eight stub agents would
produce eight vacuous gates and a worse repo than shipping none.

## 3. Two rows the tracker gets wrong — both ALREADY SHIPPED AND GATED

| item | tracker says | actually |
|---|---|---|
| **F#68 metric lockbox e2e** | *"kernel ✅ Python ✅ — e2e wiring unverified"*, gate `smoke-lockbox-e2e` | **Shipped and gated.** `user/lockboxtest.c` reads the lockbox through `SYS_METRIC_READ` (NSI 76) and the verification happens in `metric_lockbox_read()` *before* any bytes are copied, so a tampered record yields `-ETAMPER` and nothing else. Gate is **`smoke-lockbox`** (shard 7, **strict**) — DDR-812. The named `smoke-lockbox-e2e` does not exist and does not need to. |
| **F#76 tamper-evident ledger** | *"⬜ not started"* | **Shipped and gated twice.** `SYS_READ_AUDIT` (NSI 37) + `SYS_VERIFY_AUDIT` (NSI 93, `-EACCES` on a broken chain), with **`smoke-auditchain`** (shard 0, strict) *and* **`smoke-auditchain-tamper`** (shard 4, strict). A ledger gated both intact and tampered is exactly what "tamper-evident" asks for. |

Neither needed building. Both rows are corrected in `BUILD_TRACKER` and
`AETHER_MASTER_FEATURES`.

## 4. One item blocked by a decision already taken

**F#74 capability discovery.** `struct tcb` carries `agent_caps` (DDR-982,
`sched.h:121`) but it is initialised to `0` at `sched.c:1120` and **never
granted**, and there is no syscall for an agent to read it — `grep -nE
"SYS_(CAP|AGENT)_"` yields only `SYS_AGENT_ROSTER` and `SYS_AGENT_METRICS`, both
observability.

So F#74 needs the per-slot authority enforcement that DDR-982 **deliberately
withdrew**, plus a new NSI. CLAUDE.md's PRE-APPROVED EXCEPTIONS already rules on
that shape — *"capability bit defined, enforcement deferred — no subsystem
path"* — and DDR-1020's predecessor logged it. Building F#74 would reverse a
standing decision, which is not a probe.

## 5. Eight that need domain behaviour that does not exist

F#66 `architect_agent`, F#67 `healer_agent` (RUFLO), F#69 `inventor_agent`,
F#70 `tournament_agent`, F#71 subconscious world model, F#72 `verifier_agent`,
F#73 sovereign NL UI, F#75 lineage memory.

Each is a *behaviour*, not plumbing, and none has a subsystem behind it:

- **F#73** needs a natural-language surface. The same two pieces that block
  Ctrl+Alt+T block it: there is no windowed terminal client, and
  `sys_exec.c:47` discards `argv`/`envp`, so a spawned client cannot be told what
  to attach to. Already recorded as BLOCKED in `NEXT_TASK_QUEUE`.
- **F#75** is the closest to buildable — agent memory (NSI 82/83) exists and a
  lineage record is a convention on top of it. But its gate would assert that a
  key round-trips, which **`smoke-agentmem` already asserts**. A second gate over
  the same mechanism measures nothing new; a real one needs a lineage *consumer*,
  and there is none.
- The remaining six are agent policies whose value is the reasoning they do.
  `agent_base` can already submit actions and, in live mode, call a model over
  the proxy socket — so the plumbing is present and the *behaviour* is the work.

**PRAX / LUMYN / AHNIS / IRIS** are separately and correctly recorded as blocked
on four pre-approved deferrals (`ACTION_EXEC_CODE`, `ACTION_BROWSE_WEB`,
`ACTION_PARSE_DOCUMENT`, `ACTION_QUERY_SCENE`) — capability plumbing is not what
stops them.

## 6. Group F final state

| item | state |
|---|---|
| Section 3C (8 types) | **CLOSED** — 6 shipped and gated, 2 deferred (DDR-1021) |
| Section 3D (#45–65) | COMPLETE, 21/21 (DDR-846–856) |
| F#68 metric lockbox | **shipped + gated** — `smoke-lockbox`, DDR-812 |
| F#76 tamper-evident ledger | **shipped + gated ×2** — `smoke-auditchain`, `smoke-auditchain-tamper` |
| F#74 capability discovery | **deferred** — needs DDR-982's withdrawn enforcement reversed |
| F#66/67/69/70/71/72/73/75 | **deferred** — domain behaviour, no subsystem; a stub would gate vacuously |
| PRAX / LUMYN / AHNIS / IRIS | **deferred** — four pre-approved action-type deferrals |
| S3 + S7 invariant arms | remain blocked on F#66–F#72, which are deferred above |

**Nothing in Group F is now both buildable and unlogged.**

## 7. What this does not claim

It does **not** claim Group F is complete as a feature set — eight agents and an
NL UI are genuinely absent, and the roster will show empty slots. It claims only
that each is either shipped and gated, or deferred with a reason recorded where
the release checklist can see it. That is the difference between a finished
assessment and a green checkbox.
