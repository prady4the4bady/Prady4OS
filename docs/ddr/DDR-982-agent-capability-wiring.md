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
