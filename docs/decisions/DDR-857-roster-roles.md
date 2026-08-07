# DDR-857 — Section G: the 12-agent roster gets behaviour

**Status:** Accepted
**Date:** 2026-08-07
**Scope:** Group 2 item 11 (Section G), and the agent halves of Section F #66
(architect), #67 (healer), #69 (inventor→ai_scientist), #70 (tournament),
#72 (verifier). Host-side Python, no kernel surface.

## Context

DDR-846 gave the eight legacy slots a `skill.md` each — a prompt saying what the
agent *is*. This adds logic: what it *does* when asked.

## Decision

**An agent is a thin, capability-gated dispatcher over subsystems that already
exist.** It routes and refuses; it does not reason.

That rule is a direct consequence of DDR-855, where I rebuilt D-07 and D-13
because I had not looked first. The pull here is identical and stronger — every
role reads like it wants its own planner, its own memory, its own verifier. So
`Role.delegates_to` is **required**, and a test **imports every named delegate**:
a role that names nothing has no legitimate implementation, and a role naming a
module that does not resolve is a promise to code that is not there, which reads
as "already integrated" to the next person.

Where a subsystem is not wired, the role declares itself **not spawnable**
rather than shipping a private half-implementation that would drift from the
real one and win by being imported first.

**The gate raises before the subsystem is touched.** A partially-executed
refused action is worse than a refused one: it has already had effects nobody
authorised. `ensure_spawnable()` then `require()` then the handler — in that
order, asserted by a mutation that reorders them.

**Declared vs granted.** `effective = declared ∩ granted`. The declaration is a
claim, the grant is the fact: a role declaring `CAP_REWRITE` but granted only
`CAP_AGENT` cannot rewrite anything, and no caller can hand an agent *more* than
its role declares.

## The roster

Eight legacy slots (KRYOS→file, PRAX→shell, LUMYN→research, AHNIS→ocr,
IRIS→vision, RUFLO→healer, HERMES→orchestrator, SOLIN→verifier) plus four new:
`subconscious_agent`, `ai_scientist_agent`, `architect_agent`,
`tournament_agent`. The four new ones have **no kernel roster slot**, which is
itself a reason they cannot be scheduled yet.

Five are **not spawnable** — `shell_agent`, `research_agent`, `ocr_agent`,
`vision_agent`, `ai_scientist_agent` — because `CAP_EXEC`, `CAP_NET_BROWSE`,
`CAP_OCR` and `CAP_SCENE` are declared in `kernel/cap.h` and wired to nothing.
The list is asserted explicitly, so wiring or unwiring a capability shows up as
a test change rather than a silent behaviour change.

## Verification

`aether/tests/test_roster_roles.py` — 23 tests, **10/10 mutations killed**,
including gate-disabled, gate-reordered-after-handler, declaration-ignores-grant
and grant-ignores-declaration.
