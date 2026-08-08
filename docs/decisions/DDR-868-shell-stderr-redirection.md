# DDR-868 — PRISM shell `2>>` and `2>&1` (Group 6 item 33)

**Status:** Accepted
**Date:** 2026-08-08
**Scope:** `user/prism.c`, `smoke-shell`. Ring-3 only — no kernel change.

PRISM already had `>`, `>>`, `<` and `2>` (DDR-778/781/782/784). Item 33 asked
for `2>&1` and `2>>`.

## Decisions

**`2>&1` takes no operand, so it is matched before the "next token is the
target" logic.** Treating it as `2>` followed by a file named `&1` would create
a junk file and point stderr at it — and it would *look* like it worked.

**`2>&1` is applied AFTER the stdout swap, and the ordering is the entire
point.** `cmd > f 2>&1` must send both streams to `f`, which only holds if fd 1
already refers to `f` when fd 2 is duplicated from it. Applied earlier it
captures the console, errors appear on the terminal, and the file is missing
exactly the diagnostics the syntax exists to collect.

It is a **snapshot, not an alias**: a later change to fd 1 does not follow. That
is the shell semantics being implemented, and stating it prevents a future
"fix" that makes fd 2 track fd 1.

**Last write wins between `2>` and `2>&1`.** An explicit file after `2>&1`
overrides it, and vice versa — matching how a real shell resolves the conflict
rather than silently applying both.

## The gate assertion was worthless, and a mutation proved it

The first `2>&1` check grepped the whole serial log for the error text. That
matches whether the error landed **in the file** or **on the console** — the two
outcomes the test exists to distinguish. A mutation that dup'd the *saved
console* fd instead of the live fd 1 (exactly what an early swap produces)
**passed**.

The fix is positional: `echo MARKER66c` runs after the redirect and before the
`cat`, so the error text must appear only in the lines *after* the marker, and
must **not** appear before it. Both directions are asserted — presence after,
absence before — because either alone is satisfiable by the wrong behaviour.

This is the same defect this project keeps finding: a check that cannot fail for
the reason it claims to test. It is worth noting it appeared in a gate I wrote
*in the same session* as DDR-867, whose whole subject was a harness that lied.

## Verification

| mutation | expected | result |
|---|---|---|
| baseline | PASS | ✅ |
| `2>>` always truncates | FAIL | ✅ `2>> truncated the earlier entry` |
| `2>&1` captures the console | FAIL | ✅ `2>&1 did not send stderr to the stdout file` |
| restore | PASS | ✅ |

An earlier mutation attempt disabled each feature by removing its variable;
`-Werror` rejected that as unused, so it never reached the gate and proved
nothing about the gate. The mutations above deliberately keep compiling.

`2>>` is checked by requiring **both** error lines to survive in one file — a
"file is non-empty" check would accept a truncating implementation.

Regression green: `smoke-shell`, `smoke`, `smoke-ftruncate`, `smoke-user`,
`smoke-init`. Zero warnings under `-Werror`.

**Group 6 item 33 complete.**
