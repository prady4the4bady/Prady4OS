# DDR-846 — the 8 legacy agents get roles and `skill.md` files

**Status:** accepted
**Date:** 2026-08-06
**Governs:** `aether/agents/roster/*/skill.md`, `aether/tests/test_agent_skills.py`
**Covers:** Section 3D #45 (per-agent `skill.md`, 300–2000 tokens)

## The problem this had to solve first

KRYOS, PRAX, LUMYN, AHNIS, IRIS, RUFLO, HERMES and SOLIN have kernel roster slots
and compositor UI cards (DDR-707/730/737) — and **no defined roles anywhere in
the repo**. The tracker records it precisely: *"8 kernel slots registered + UI
cards; skill prompts ❌"*. They are eight labelled buttons wired to nothing.

Writing a `skill.md` therefore required deciding what each agent IS, which is a
design decision, not a documentation task.

## Decision — map the 8 legacy names onto Section G's 8 highest-priority roles

Section G specifies a 12-agent roster of *new* agents (`file_agent`,
`shell_agent`, …). The obvious-but-wrong move is to build those 12 alongside the
legacy 8, leaving sixteen agents where the eight named ones do nothing and the
twelve real ones have no UI slot.

Instead the legacy names **become** the first eight Section G roles:

| legacy | role | why this name | capabilities |
|---|---|---|---|
| KRYOS | `file_agent` | already the daemon's default slot-0 agent; file R/W is the floor every other agent stands on | CAP_AGENT, CAP_MEMORY |
| PRAX | `shell_agent` | *praxis* — doing, execution | CAP_AGENT, CAP_EXEC |
| LUMYN | `research_agent` | illumination, discovery | CAP_AGENT, CAP_MEMORY, CAP_NET_BROWSE |
| AHNIS | `ocr_agent` | document → structure | CAP_AGENT, CAP_OCR |
| IRIS | `vision_agent` | the eye | CAP_AGENT, CAP_SCENE **(post-L7)** |
| RUFLO | `healer_agent` | restoring flow | CAP_AGENT, CAP_REWRITE |
| HERMES | `orchestrator_agent` | the messenger; coordination | CAP_AGENT, depth-0 spawn |
| SOLIN | `verifier_agent` | independent checking | CAP_AGENT |

Section G's remaining four — `subconscious_agent`, `ai_scientist_agent`,
`architect_agent`, `tournament_agent` — extend the roster to 12 under item 11.
That makes item 11 an *extension* of a working roster rather than a second one.

## Each `skill.md` states what the agent may NOT do

A skill prompt that only describes capability is a wish. Every file carries an
explicit **Refuses** section naming the actions the agent must decline, tied to
the invariant that forbids them — because the failure mode of an agent prompt is
not "does too little", it is "talks itself into something S1–S8 forbids".

Where a capability does not yet exist in the kernel (`CAP_OCR`, `CAP_EXEC`,
`CAP_SCENE`, `CAP_NET_BROWSE` are all unwired), the file says so plainly and the
agent is marked **not yet spawnable**. A skill file implying a live capability
that returns `-EPERM` would be a lie the agent acts on.

## Validation is a test, not a convention

`aether/tests/test_agent_skills.py` runs inside the pytest job CI already
executes with `-W error`, and asserts:

1. every one of the 8 has a `skill.md`
2. each is within the spec's **300–2000 token** budget (approximated at 0.75
   words/token, with the estimator's assumption stated in the test)
3. each declares `Role`, `Capabilities`, `Refuses`, and `Invariants`
4. the `Refuses` section is non-empty
5. no file claims a capability the kernel has not wired without also carrying the
   `not yet spawnable` marker

Point 5 is the one that matters: it is the check that stops a future edit from
quietly promising `CAP_OCR` behaviour that cannot run.

## The rule this earns

**A named slot with no defined behaviour is worse than an empty list.** Eight UI
cards implied eight working agents for several layers of development; nobody
noticed because a label cannot fail a test. Defining the role and testing the
definition are the same act.
