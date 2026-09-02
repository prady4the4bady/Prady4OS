# DDR-1039 — PRISM discards nothing on backspace: the byte lands in the command

**Status:** DESIGN (implementation follows)
**Date:** 2026-09-02
**Queue:** Group D, "PRISM line discipline" — the row DDR-1038's pass corrected from *"readline / line discipline / echo"* once `readline()` was found to exist.

---

## §1 — The defect, in one line

`readline()` (`prism.c:92`) copies **every** non-`\n`, non-`\r` byte into the
buffer. A backspace is `0x7F` (DEL) or `0x08` (BS) — both are "every byte" — so
typing `hepl`⌫⌫`lp` yields `hepl\x7f\x7flp`, which matches no builtin and
reports an unhelpful error about a command the user did not type.

**Why it has gone unnoticed:** every gate that drives PRISM injects a
byte-perfect line and never sends an erase, so no gate has ever typed a typo.
The defect is invisible to the entire test suite and visible to any human on
the first mistyped command.

## §2 — Scope, and what is deliberately NOT in it

**IN: erase handling only.** `0x08` and `0x7F` remove the previous character
from the buffer; at column zero they do nothing (they must not underflow `n`,
and must not erase the prompt).

**OUT: echo.** `sys_io.c:301` states it plainly — *"No echo / line discipline
here"* — and neither the kernel console read nor PRISM echoes. Adding echo is a
**separate and riskier change**, and it is separated deliberately:

1. It would put typed input into the **serial log**, which is what every gate
   asserts on. `smoke-shell` alone has five arms over that stream. A change that
   rewrites the thing 170 gates measure is not a line-discipline tidy-up.
2. Without termios there is no way to tell whether the user's terminal is
   already echoing locally, so PRISM echoing risks **double** characters for the
   common serial setup rather than fixing anything.

**Erase alone is still a real fix for the common case:** a serial terminal with
local echo shows the user their own typing and their own backspace, and it is
only PRISM's *parse* that is wrong. This corrects the parse without touching the
output stream.

## §3 — The gate arm, and why it cannot be a new gate

`smoke-shell` already drives PRISM through `input_inject`-style line feeding and
asserts on its output. The arm is one more line typed **with an embedded erase**,
asserting the command still runs:

> feed `hepl\x7f\x7flp\n` and assert the `help` output appears.

**It must be an arm on `smoke-shell`, not a new gate**, because the property is
"PRISM parses a line containing an erase correctly", and `smoke-shell` is where
PRISM's line handling is already exercised. A separate gate would boot a whole
OS to type one word.

**The anti-vacuity requirement:** the arm must feed a line whose *erased* form
differs from its literal form in a way the assertion can see. `hepl\x7f\x7flp`
becomes `help`; the literal string matches no builtin, so a PRISM that ignores
the erase prints an error and the arm fails. Feeding `help\x7f` and expecting
`help` would NOT work — the erased form would be `hel`, and neither form is
`help`.

## §4 — Mutant

**M1 — restore the original byte-append.** The arm must fail: PRISM receives
`hepl\x7f\x7flp`, matches nothing, and the `help` output never appears.

One mutant is the right number here: there is exactly one behaviour (erase
removes the previous byte) and one arm testing it. A second mutant would have to
target the column-zero guard, which is unobservable from the shell — erasing at
column zero and erasing nothing look identical from outside. **That guard is
therefore recorded as uncovered**, in the same terms DDR-1031 used for its
`invlpg` and DDR-1036 §5.1 for the parser refusal.

## §5 — What this does NOT fix

- **No echo** (§2). The user still sees only what their own terminal echoes.
- No cursor movement, no left/right arrows, no word-erase (`^W`), no kill-line
  (`^U`), no history. Those are the rest of a line discipline and each would
  need the same scrutiny about the output stream.
- Nothing in the kernel changes. This is entirely inside `user/prism.c`.
