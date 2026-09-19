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

`smoke-shell` already feeds PRISM a line at a time through a FIFO and asserts on
its output. The arm is one more fed line **with an embedded erase**, asserting
that the *erased* form is what ran.

**It must be an arm on `smoke-shell`, not a new gate**, because the property is
"PRISM parses a line containing an erase correctly", and `smoke-shell` is where
PRISM's line handling is already exercised. A separate gate would boot a whole
OS to type one word.

**The anti-vacuity requirement:** the arm must feed a line whose *erased* form
differs from its literal form in a way the assertion can see.

### §3.1 — CORRECTION: the design's own first proposal was VACUOUS

This section originally specified:

> feed `hepl\x7f\x7flp\n` and assert the `help` output appears.

**That arm could not have failed.** `smoke-shell` already feeds a plain `help`
near the top of the same session (`Makefile:1509`), so `builtins: help echo …`
is in the serial log either way — the assertion would have passed on a PRISM
with no erase handling at all. Ninth instance of the dead-arm class, and the
first one caught in a DDR's own design text before any code was written.

The shipped arm feeds a line whose output marker exists **only** in the erased
form:

| | fed bytes | what runs | printed |
|---|---|---|---|
| erase honoured | `echo erasX<0x7F>e-ok-3m7` | `echo erase-ok-3m7` | `erase-ok-3m7` |
| erase ignored | same | `echo erasX<0x7F>e-ok-3m7` | `erasX<0x7F>e-ok-3m7` |

`erase-ok-3m7` appears in no other line of the suite, and `erasX` appears only
in the *unerased* form. Both directions are asserted: presence of the marker,
and absence of `erasX`. Neither alone pins the position — a shell that dropped
the byte without decrementing would print `erasXe-ok-3m7` (no marker, `erasX`
present), and one that decremented twice would print `eraXe-ok-3m7` (no marker,
no `erasX`). Requiring both is what makes the arm test *erase*, not *strip*.

## §4 — Mutant, MEASURED

**M1 — disable the erase branch** (`if (c == 0x7F || c == 0x08)` -> `if (0)`,
so the byte falls through to the plain append, which is the pre-fix code).

| build | kernel.bin | `smoke-shell` |
|---|---|---|
| clean | `8212d26ef58544b0` | **rc=0, PASS 5/5** |
| M1 | `a411e1b1b765e15e` | **rc=2**, `FAIL: backspace not honoured — erased form never ran (DDR-1039)` |

`prism.elf` is embedded in `kernel.bin` (`user_image.o`), so the mutation binds
to the kernel hash — unlike DDR-1027's `term.elf`, which is loaded from the FAT
volume and leaves `kernel.bin` bit-identical. The clean tree rebuilt to
`8212d26ef58544b0` after M1 was reverted, so the two hashes are the only
difference between the two runs.

The M1 serial log carries the defect verbatim:

```
prism> erasX^?e-ok-3m7
```

— the raw DEL byte sitting inside the command buffer, echoed back out by `echo`,
which is the whole defect in one line.

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
