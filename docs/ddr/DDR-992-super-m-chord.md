# DDR-992 — Super+M sovereign toggle, and why a chord must not also type text

**Status:** design + implementation.
**Depends on:** DDR-991 (KMOD_META did not exist before it).
**Relates:** DDR-893 (Manual is its own desktop), ADR-026 D2 (sovereign default).

---

## 1. The conflict this item walks into

The compositor's key loop dispatches on bare ASCII: `'s'` selects Sovereign,
`'m'` selects Manual, plus `q/p/b/r/k`. The queue asks for **Super+M** as a
physical sovereign-mode *toggle*.

Naively adding that breaks immediately. Super+M still produces an ordinary `'m'`
make code — the modifier does not change the base scancode — so with DDR-991's
event ring added alongside the existing byte stream, **one keypress arrives
twice**: once as `KEY 'm'` with `KMOD_META` on NSI 96, and once as a bare `'m'`
byte on NSI 46. The compositor would run the toggle *and* the plain-`'m'`
branch, so Super+M would flip the mode and then immediately force it to Manual.
The observable result is "Super+M sometimes does nothing", depending on which
stream is drained first — an ordering-dependent bug, the kind that survives a
gate written after the fact.

## 2. The fix is not to special-case 'm'

The tempting patch is "ignore `'m'` on the byte stream while Meta is held". That
cannot be written correctly: **the byte stream carries no modifier information**
— that is precisely why DDR-991 added a second ring — so the ASCII consumer has
nothing to test. Any version of it reconstructs modifier state out of band and
races the release.

The real statement is more general:

> **A chord is not text.** `Ctrl+C` is an interrupt, not the letter `c`.
> `Super+M` is a mode toggle, not the letter `m`.

So the driver stops pushing to the ASCII ring when a **non-Shift** modifier is
held. Shift is explicitly excluded because Shift *is* a text modifier — it
selects the shifted glyph, which is what `map_upper` exists for.

This is also a latent-bug fix independent of Super+M: today `Ctrl+C` types a
literal `c` into whatever is reading NSI 46, which is wrong on its own terms and
was simply never exercised, because nothing sent chords until DDR-991 made them
representable.

## 3. Scope, and what is deliberately not changed

`'s'`, `'m'`, `q/p/b/r/k` keep their exact current meaning as unmodified keys —
`smoke-compositor` depends on `m`/`s`, and this must not disturb it. Super+M is
added as a *toggle* (read the current mode, write its inverse) rather than a
third way to select a fixed mode, because that is what the queue item asks for
and because a toggle is what a physical key on a real keyboard should do.

## 4. Gate

`smoke-superkey`, four arms:

- **A** plain `m` still selects Manual — the DDR-893 behaviour is untouched.
- **B** `Super+M` toggles: from Manual it must reach Sovereign.
- **C** a second `Super+M` toggles back — proving it is a toggle and not a
  disguised "set Sovereign".
- **D** `Ctrl+C` does **not** deliver a `c` byte on NSI 46 — the §2 rule, and
  the arm that fails if someone "simplifies" the driver back to always pushing.

Arm D is the one that would be dropped by a careless later edit, which is
exactly why it is written down as an arm rather than left as a comment.

---

# IMPLEMENTED

## 5. Result

`smoke-superkey`: **PASS — toggles both ways.**

```text
PRADYOS_SUPERKEY_TOGGLE from=1 to=0
PRADYOS_SUPERKEY_TOGGLE from=0 to=1
```

Both directions appear, which is what arm C set out to prove: a fixed
"set Sovereign" binding could only ever produce one direction.

**A limit of this gate, stated rather than left implicit.** The injector sends
four rounds, so eight Super+M presses occur, and the recipe only asserts that
each direction appears *somewhere* in the log. It therefore proves the binding
toggles; it does **not** prove strict alternation, and a hypothetical defect that
toggled correctly only on even presses would pass. Tightening that needs an
injector that sends a known count with known spacing, which the shared
`input_inject.sh` does not do. Recorded as a known bound on what this gate
claims, not smoothed over.

## 6. Arm D moved where both streams are visible

§4 listed arm D — a chord must not deliver text — but the compositor never
prints the bytes it receives, so `smoke-superkey` cannot see it. It is therefore
implemented as **arm F of `smoke-modkeys`**, which already drains *both* NSI 46
and NSI 96: that gate injects `ctrl-c`, and nothing else in its key sequence
produces a `c`, so a bare `c` on the byte stream is unambiguous evidence that a
chord emitted text.

Putting the assertion in the gate that can actually observe it matters more than
keeping it in the DDR section that first named it.

## 7. Regression surface

The §2 change alters NSI 46 semantics for every consumer, so the whole input
surface was re-run: `smoke-modkeys`, `smoke-superkey`, `smoke-input`,
`smoke-compositor`, `smoke-shell`.

The change is narrow by construction — it suppresses the byte only while
**Ctrl, Alt or Meta** is held, and Shift is deliberately excluded because Shift
is a text modifier. Unmodified typing, which is all any existing gate sends, is
bit-for-bit unaffected.

---

## 8. The suppression is only as correct as the aggregate (DDR-993)

Written after the fact, because it is not obvious from §2 and it cost a real
defect.

§2's suppression is one line:

```c
if (g_mods & (KMOD_CTRL | KMOD_ALT | KMOD_META))
    return;                       /* a chord is not text */
```

Everything in this DDR therefore rests on `g_mods` being **true while the key is
physically held**. It was not. DDR-991's `mods_set` cleared the aggregate on the
break of *either* side of a pair, so releasing one Ctrl while the other was held
made `KMOD_CTRL` read 0 — and this line stopped firing. **A chord typed text
again**, which is precisely what §1 called the conflict and §2 called fixed.

The defect predates this DDR by one commit, so nothing here caused it; but this
DDR is what turned a wrong-glyph bug into a correctness hole, and neither DDR's
gate could see it (DDR-993 §5 — all six ring-3 arms pass on the broken kernel).

Fixed in DDR-993 by deriving the aggregate from per-side state so it cannot
disagree with its sides. **The dependency is worth stating plainly: a change to
modifier tracking is a change to chord suppression.**
