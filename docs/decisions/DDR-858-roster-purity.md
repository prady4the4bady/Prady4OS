# DDR-858 — CI red on `934106c`: code in a data directory

**Status:** Accepted
**Date:** 2026-08-07
**Scope:** Fixes the regression DDR-857 introduced. No kernel surface.

## What broke

Runs `31134985230` and `31134986902` on `934106c` both failed
`test_agent_skills.py::test_every_kernel_roster_slot_is_covered`:

    roster/ holds ['__pycache__', 'ahnis', ..., 'solin'],
    kernel slots are ['ahnis', ..., 'solin'] — these must not drift apart.

DDR-857 put `roles.py` inside `aether/agents/roster/`. Importing it creates
`__pycache__` there, and DDR-846's slot test lists directories in `roster/` and
compares them to the kernel's eight — so a bytecode cache read as a ninth agent.

The test was right. My change was wrong.

## Why I did not catch it

**I was not running `test_agent_skills.py` locally.** My runner covered the
suites I had written this session and none of the pre-existing ones, so a
regression in somebody else's test could only surface in CI. That is the whole
value of a test suite defeated by which files I chose to execute.

Worse, my shim treated `@pytest.mark.parametrize` as a no-op, so **five
parametrized tests in that file were reported "skipped" and never ran**. Honest
reporting, but still five tests I was not executing.

## Decision

**1. `roster/` is a data directory and stays one.** `roles.py` moved to
`aether/agents/runtime/roles.py`, beside `agent_bus.py` (B-07) — which the
orchestrator role delegates to anyway, so it is the more coherent home.

**2. The rule is enforced, not remembered.**
`test_roster_holds_no_python_so_no_bytecode_dir_can_appear` fails if any `.py`
appears under `roster/`.

Asserted at the source rather than teaching the slot test to skip
`__pycache__`. An exception there would also hide a genuinely stray directory,
and the real rule is that code does not belong in a data directory — so that is
what gets checked. Verified by copying a module back in: the new test fails, and
only that test.

**3. The local runner now covers the pre-existing suites and expands
parametrize.** Fifteen suites, **406 tests, zero skipped** — up from nine suites
and 260 while five tests sat silently unexecuted.

## Verification

| | before | after |
|---|---|---|
| suites run locally | 9 | 15 |
| tests | 260 | 406 |
| skipped | 6 | 0 |

Roster mutations re-run after the move: 10/10 killed.
