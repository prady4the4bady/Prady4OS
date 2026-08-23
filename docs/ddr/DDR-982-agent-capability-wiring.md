# DDR-982 — CAP_OCR / CAP_EXEC / CAP_SCENE / CAP_NET_BROWSE: ship the boundary, name the gap

Status: PROPOSED — design + a scope finding the operator should decide on.
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§INV.4).

**Answers:** operator directive 2026-08-23 §3, *"wiring `CAP_OCR`/`CAP_EXEC`/
`CAP_SCENE`/`CAP_NET_BROWSE` so PRAX, LUMYN, AHNIS, and IRIS are actually
spawnable."*

---

## 1. The finding, before any code

The directive suspends the PRE-APPROVED EXCEPTIONS table and says nothing in
§3 may be silently deferred. Taking that seriously means checking, first, what
each of these four capabilities would actually gate. Three of the four gate an
action whose **implementation** is out of scope by the directive's own §2, or
needs an asset that cannot ship:

| capability | agent | gates | status of the thing it gates |
|---|---|---|---|
| `CAP_EXEC` (1<<20) | PRAX (shell_agent) | `ACTION_EXEC_CODE` | needs a sandboxed interpreter subsystem — **buildable**, `execve` exists |
| `CAP_NET_BROWSE` (1<<23) | LUMYN (research_agent) | `ACTION_BROWSE_WEB` | **directive §2 keeps the cloud bridge (DDR-793) deferred by name** |
| `CAP_OCR` (1<<19) | AHNIS (ocr_agent) | `ACTION_PARSE_DOCUMENT` | needs a 64 MiB OCR model; there is no model-shipping path |
| `CAP_SCENE` (1<<22) | IRIS (vision_agent) | `ACTION_QUERY_SCENE` | needs a scene graph (SLAM3R); no subsystem exists |

**This is not an argument for skipping the work.** It is a distinction between
two things the directive's phrasing runs together:

- **"are actually spawnable"** — an agent holding a capability, occupying its
  roster slot, running, and being *denied* the actions it is not authorised for.
  This is entirely buildable now, for all four, and it is the security boundary.
- **the action implementations behind three of those capabilities** — OCR,
  scene query, and web browse. One of those (browse) the directive itself keeps
  deferred; the other two need assets this repo has no way to ship.

So §2 is satisfiable in the sense that matters, and the residue is named here
rather than folded into a table.

## 2. What this DDR proposes to build

**A. The four bits.** `cap.h` gains 19/20/22/23 (21 is `CAP_REWRITE`, already
taken — the free bits are exactly the four the directive names, which is why it
names them).

**B. Per-roster-slot authority.** Today `aether_spawn_agent_hook`
(`main.c:970`) loads one `agent_base_elf` for every agent and marks them all
identically `is_agent` + `is_net`. Every agent therefore has the same authority,
which is precisely what a capability system exists to prevent. The hook gains a
roster slot argument and mints per-slot caps:

| slot | agent | capabilities beyond the common set |
|---|---|---|
| 1 | PRAX | `CAP_EXEC` |
| 2 | LUMYN | `CAP_NET_BROWSE` |
| 3 | AHNIS | `CAP_OCR` |
| 4 | IRIS | `CAP_SCENE` |

**C. Enforcement at action dispatch**, not at the action implementation. The
check belongs where `sys_aether.c` already does its six `cap_ok` tests, so an
unauthorised submit is denied and audited (`AR_CAP_DENIED`) *regardless* of
whether the action behind it is implemented. That ordering matters: it means
the boundary is testable today and stays correct when the implementations land.

**D. The gate — `smoke-capagent`.** Two arms, and the second is the one that
makes it non-vacuous:
1. **grant**: each of the four agents spawns, claims its slot, and submits its
   own action type → accepted.
2. **denial**: each submits *another* agent's action type → `AR_CAP_DENIED`,
   with the audit record present. A grant-only gate would pass against a kernel
   that ignores capabilities entirely, which is the DDR-973 vacuity lesson.

Risk tier **strict** (§6.3): this is the capability system, so N=20, not N=5.

## 3. What this DDR explicitly does NOT claim

- It does **not** make AHNIS parse a document, IRIS answer a scene query, or
  LUMYN browse the web. Those need, respectively, a 64 MiB model, a scene
  graph, and the cloud bridge the directive keeps deferred.
- Shipping a capability bit whose action is unimplemented is exactly the
  *"capability bit defined, enforcement deferred"* exception the directive
  suspended — **which is why enforcement (C) and the denial arm (D2) are the
  load-bearing parts here, not the bit definitions.** A bit with a real,
  audited denial path is not a deferred capability; a bit with no check is.

## 4. Open question for the operator

`ACTION_EXEC_CODE` is the one with a real path. Its listed blocker is "needs a
sandboxed interpreter subsystem", and PRAX is a *shell* agent — so the question
is whether PRAX may drive `sys_execve` against the existing W^X + capability
machinery (an agent spawning a ring-3 process under `CAP_EXEC`), or whether a
separate sandbox is required first. That is a security-posture decision of the
same kind DDR-793 made for the cloud bridge, and it is not mine to make
unilaterally. Sections A–D above are buildable and correct either way; only
the *content* of `ACTION_EXEC_CODE` depends on the answer.

---

## 5. CORRECTION — §2(C) as written cannot be built, and the repo already argued why

Implementing §2 turned up a documented decision that contradicts this DDR's own
reasoning. Recording it rather than quietly working around it.

### 5.1 The four action types do not exist, deliberately

`aether.h:22-30`:

> *Six further 3C types are deliberately ABSENT until their subsystem exists —
> CAPTURE_FRAME / SCAN_ENVIRONMENT / QUERY_SCENE (post-L7, `CAP_SCENE`),
> PARSE_DOCUMENT (64 MiB OCR model), EXEC_CODE (sandboxed interpreter),
> BROWSE_WEB (headless browser + deferred cloud bridge). **Declaring an enum
> value with no enforcement is worse than omitting it: an agent could submit one
> and the kernel would queue an action nothing implements.***

So there is no `ACTION_PARSE_DOCUMENT` to gate on `CAP_OCR`, and likewise for the
other three. §2(C) said "enforcement at action dispatch"; there is nothing at
dispatch to enforce against.

### 5.2 That rationale is the inverse of §2's, and it is the better one here

§2 argued: ship the boundary now, implementations later — a denial path with an
audit record is a real capability. `aether.h` argues: a *type* with no subsystem
is worse than no type, because the kernel would accept and queue work nothing
performs.

Both are coherent, but they are about different objects, and on the object in
question `aether.h` is right. My argument holds for a **capability bit** whose
check runs and denies. It does not hold for an **action type**, because
declaring one creates a submission path that succeeds and enqueues a record that
never completes — an agent would see `AE_PENDING` forever, which is worse than
`-EPERM`. The capability boundary I wanted is real; the vehicle I chose for it
is not available without also creating that failure mode.

There is a wire-format cost too, and DDR-832/DDR-842 already priced it:
`action_type` crosses the ring boundary in every audit record and queue entry.
Appending is safe; the four types would have to be appended in a fixed order and
pinned with `_Static_assert`s like the existing ones. That is not free and
should not be spent to gate actions three of which cannot be implemented.

### 5.3 What is built and kept (A and B), and what is withdrawn (C, D)

**Kept — built, compiles clean, `kernel.bin` 1,065,350 B unchanged:**

- **(A)** `CAP_OCR` (1<<19), `CAP_EXEC` (1<<20), `CAP_SCENE` (1<<22),
  `CAP_NET_BROWSE` (1<<23) in `cap.h`. Bit 21 is `CAP_REWRITE`, which is why the
  directive names exactly these four.
- **(B)** `tcb.agent_caps`, a per-agent authority mask, explicitly zeroed in
  `sched_create` (§NON-NEGOTIABLE 10 — `kmalloc` does not zero, and a garbage
  mask would grant an agent every capability at once).

**(B) is worth having on its own merits, independent of this DDR's purpose.**
`aether_spawn_agent_hook` (`main.c:970`) currently marks *every* agent
identically `is_agent = 1; is_net = 1`. All eight roster agents hold identical
authority today — precisely the condition a capability system exists to prevent.
Per-agent authority is the prerequisite for fixing that, whatever gates it.

**Withdrawn pending a decision:** §2(C) enforcement and §2(D) `smoke-capagent`.
Both require the action types to exist. A gate written now could only assert
that a bit can be set and read back, which is a test of `uint32_t`, not of a
capability system — the DDR-973 vacuity lesson applied to my own proposal.

### 5.4 One more thing found while implementing: a create-then-init race

`sys_spawn_agent` (`sys_aether.c:183`) already receives the roster slot as `a3`,
so no ABI change is needed to grant per-slot authority. But it calls
`g_spawn_hook(task)` — which ends in `sched_unblock(ut)` — and only *then*
records the slot. **The agent is runnable before its slot is known**, so any
per-slot authority minted afterwards would land after the first run.

That is the DDR-964 create-then-init race in a second place, and the hook's own
comment already states the requirement it violates: *"marks the new process
CAP_AGENT so it is rate-limited"* … `ut->is_agent = 1; /* authority BEFORE the
first run */`. The fix is §INV.16's pattern — pass the slot into the hook, mint
authority, then `sched_unblock` from the caller. **Not fixed here**, because it
is only observable once there is per-slot authority to race against, and that
depends on the decision below. Recorded so it is not rediscovered.

### 5.5 The decision, now sharper than §4

§4 asked whether PRAX may drive `sys_execve` under `CAP_EXEC`. The real question
is one level up:

> **Should the four absent action types be declared, accepting that three of
> them can only ever return "not implemented", in order to have a capability
> boundary to enforce?**

- **Declare them** → §2(C)/(D) become buildable, the four agents become
  spawnable-with-distinct-authority, and three action types exist that always
  fail. Costs four wire-format slots and reverses `aether.h`'s stated decision.
- **Leave them absent** → `aether.h` stands, (A)+(B) ship as the groundwork, and
  Group F's four agent rows stay honestly blocked on their subsystems rather
  than on capability plumbing.

I lean to **leave them absent** and say so plainly in the Group F rows, per
directive §2's "name it explicitly with a reason". But this reverses a
documented repo decision either way, so it is the operator's call, not mine.
