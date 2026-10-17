# DDR-995 — Alt+Tab, and giving Tab back to applications

**Status:** IMPLEMENTED, GATED, MUTATION-CHECKED BOTH WAYS.
**Supersedes (in part):** DDR-720 (plain Tab as the cycle hotkey).
**Depends on:** DDR-991 (`SYS_KEY_POLL`, NSI 96), DDR-992 (chord suppression),
DDR-993 (the modifier aggregate that both of those rest on).
**Gate:** `smoke-alttab`, extended from one arm to three.

---

## 1. The defect DDR-720 shipped with, stated plainly

`user/compositor.c:1136` treats a bare `\t` on the DDR-703 byte stream as the
window-cycle hotkey:

```c
else if (c == '\t') { /* raise the bottom-most visible surface */ }
```

The branch is unconditional and terminal, so **no application on this system can
ever receive a Tab character.** Not a text editor, not PRISM, not a form. Tab is
swallowed by the window manager before the focus routing on the next line
(`else if (focus_id >= 0) … SYS_SURFACE_SENDKEY`) can ever see it.

DDR-720 knew this was provisional — its own comment says "Tab is a compositor
hotkey, not forwarded to the focus" — and the backlog has carried "Alt-Tab with
modifier plumbing" as the fix ever since. It could not be written then: the byte
stream carries **no modifier state**, so `\t` with Alt held and `\t` alone are
the identical byte. There was no way to tell a chord from a keystroke.

## 2. Why it is writable now

DDR-991 added a second input path carrying `mods` sampled **at the event**, and
DDR-992 made a non-Shift chord stop emitting text at all. Together they make the
two cases disjoint at the source:

| physical keys | NSI 96 event | NSI 46 byte |
|---|---|---|
| Tab | `code=0x09 mods=0` | `\t` |
| Alt+Tab | `code=0x09 mods=KMOD_ALT` | **nothing** (DDR-992 §2) |

So the compositor can bind the chord without ever inspecting the byte stream for
it, and the byte stream's `\t` is unambiguously a keystroke meant for whoever has
focus. **This is the payoff DDR-991 §9 listed as "what is now unblocked", and it
is the first item to actually collect it.**

Note the dependency chain is load-bearing and recent: if DDR-993's aggregate were
still broken, `KMOD_ALT` could read clear while Alt was physically held, and
Alt+Tab would both fail to cycle *and* type a Tab into the focused app — the
worst of both behaviours. Alt+Tab is exactly the kind of two-modifier-key usage
(Alt held across a Tab press and release) that DDR-993 §1's defect corrupts.

## 3. The change

1. Move the cycle to the key-event loop the Super+M handler already runs
   (`compositor.c:1086`), keyed on `code == KEY_TAB && (mods & KMOD_ALT)`.
   `surfs[]` and `ns` are already in scope there (line 992), so the selection
   logic moves unchanged.
2. **Delete** the `c == '\t'` branch from the byte-stream loop. Tab then falls
   through to the existing `else if (focus_id >= 0)` and is delivered to the
   focused surface's key ring like any other character.

The cycle algorithm itself is untouched — still "raise the bottom-most visible
surface, skipping minimised ones". This DDR changes **what triggers it**, not
what it does.

## 4. The gate must change with it, and that is the risky part

`smoke-alttab` today injects a plain `tab` and asserts two `PRADYOS_WM_CYCLE`
lines over two distinct ids. After this change a plain Tab correctly produces
**zero** cycles, so the gate as written would fail on correct behaviour.

Rewriting a gate to match a change is exactly how a regression gets blessed, so
the rewrite is constrained: the existing assertion is **kept verbatim** and
re-pointed at `alt-tab`, and a second arm is added for the behaviour that was
previously impossible. Three arms:

- **A — Alt+Tab cycles.** `sendkey alt-tab` twice; ≥2 `PRADYOS_WM_CYCLE` lines
  over ≥2 distinct ids. This is DDR-720's assertion, unchanged except for the
  key.
- **B — plain Tab reaches the application.** `sendkey tab`;
  `PRADYOS_FOCUS_KEY … code=9` must appear. This is the defect in §1, asserted
  directly: it cannot pass on today's kernel.
- **C — plain Tab does NOT cycle.** No new `PRADYOS_WM_CYCLE` after the plain
  Tab. Without C, a compositor that bound *both* Tab and Alt+Tab to the cycle
  would pass A and B together, and the §1 defect would survive in half.

Arm C is the one that makes this non-vacuous, and it exists because DDR-993 §5
is three hours old: a gate only tests what it can express, and A+B alone cannot
express "and nothing else happened".

`surfacetest.c` prints `PRADYOS_FOCUS_KEY id=N ch=%c`; a literal tab is not
greppable, so the line gains `code=%ld`. `smoke-focus` matches only the bare
`PRADYOS_FOCUS_KEY` prefix, so it is unaffected — checked, not assumed.

## 5. Mutation check (required)

- **M1** — restore the `c == '\t'` cycle branch *in addition to* the chord bind.
  Arm C must fail while A and B still pass. This is the "bound both" mutant, and
  it is the whole reason arm C exists.
- **M2** — bind the chord without removing the byte-stream branch's swallow
  (i.e. keep `\t` from reaching focus). Arm B must fail.

A mutant that passes all three arms means the gate is decoration.

## 6. What is NOT claimed

- Not a full Alt+Tab: no MRU ordering, no held-Alt overlay with repeated Tab
  presses to walk a list, no Shift+Alt+Tab reverse. It cycles one step per press,
  exactly as DDR-720's did. Those are separate items and none of them is on the
  release path.
- Not a claim that every hotkey should move to NSI 96. The single-letter
  compositor keys (`s`/`m`/`q`/`p`) stay on the byte stream deliberately — they
  are the gate-driven control surface, and moving them would churn a dozen gates
  for no behavioural gain.
- `KEY_TAB` is `0x09`, which is also `map_lower[0x0F]`'s ASCII, so the event's
  `code` and `ascii` agree here. That is a coincidence of Tab being a control
  character with a printable-table entry, not a general rule — DDR-993 §4 is
  explicit that `code` is an identity and `ascii` is a convenience.


---

## 7. Results (measured 2026-08-24, kernel `82fcac7d3117c63b`)

Gate `smoke-alttab`, three arms, all green:

```
[alttab] run 1/2 — Alt+Tab (arm A)
[alttab] arm A PASS — 4 cycles over 2 windows
[alttab] run 2/2 — plain Tab (arms B + C)
[alttab] arm B PASS — plain Tab delivered to focus
[alttab] arm C PASS — plain Tab did not cycle
```

### 7.1 The gate is two boots, not one — and §4's design was wrong

§4 assumed one boot could carry all three arms by injecting
`alt-tab alt-tab tab` and checking the cycles all pre-date the Tab. **That does
not work, and the reason is in the injector, not the compositor:**
`tools/qemu_runner/input_inject.sh` repeats the entire key sequence four times
(`for _round in range(4)`). After round 1, a plain Tab always precedes the next
Alt+Tab, so ordering within the log carries no information at all — a delta-based
arm C would have been noise dressed as an assertion.

Splitting into two sequential boots (never concurrent — §NON-NEGOTIABLE 12) makes
arm C an **absolute**: in a boot where only a plain Tab was ever pressed, the
correct number of window cycles is exactly zero. Stronger than the design, and
immune to the injector's repetition. `smoke-alttab`'s shard budget went 120 → 240 s
to pay for the second boot.

The §4 error is worth recording rather than quietly fixing: it was a claim about
timing made without reading the tool that produces the timing, which is the same
class of mistake as §INV.8.

### 7.2 Mutation check — both mutants behaved as precommitted

| mutant | kernel | arm A | arm B | arm C |
|---|---|---|---|---|
| **M1** — bound to BOTH Tab and Alt+Tab | `7fc64e0bfe7e1e3c` | PASS | PASS | **FAIL** (plain Tab cycled 3×) |
| **M2** — DDR-720's swallow restored | `4a82daecf6d0fa15` | PASS | **FAIL** | — |
| fixed | `82fcac7d3117c63b` | PASS | PASS | PASS |

**M1 is the entire justification for arm C**, and it is now measured rather than
argued: arms A and B both pass on that mutant. A two-arm gate would have called
the half-fixed compositor green.

Each mutant was built and hash-checked as a distinct kernel, and the restored
tree rebuilt to `82fcac7d3117c63b` — byte-identical to the kernel the three arms
passed on, so the green result belongs to the shipped code and not to a stale
build (the DDR-990 §8 precaution).

### 7.3 Regression surface

`smoke-shell` 5/5, `smoke-focus`, `smoke-compositor`, `smoke-modkeys`,
`smoke-superkey`, `smoke-blkmq`, `smoke-yieldstall` all PASS on the shipped
kernel. `smoke-focus` matters specifically: it consumes the
`PRADYOS_FOCUS_KEY` line this DDR extended with `code=`, and §4 predicted it was
unaffected because it matches only the bare prefix. Verified, not assumed.
`smoke-modkeys` and `smoke-superkey` matter because they exercise the same two
input rings this change re-partitions.
