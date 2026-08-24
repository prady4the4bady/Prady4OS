# DDR-991 — PS/2 modifiers, extended scancodes, and a structured key ABI

**Status:** design + implementation.
**Unblocks:** four Group E queue items — Super+M sovereign toggle, Alt-Tab with
real modifier plumbing, Ctrl+Alt+T, and per-window keyboard routing. None of
them can be built on today's driver, which is why they are batched behind this.
**Relates:** DDR-703 (the original ASCII ring), DDR-720 (plain-Tab switcher).

---

## 1. What the driver can currently see — which is less than it looks

`ps2kbd.c` (DDR-703) does the minimum, and the minimum turns out to exclude
every key these four features need:

| input | today |
|---|---|
| Shift | tracked (`g_shift`) |
| **Ctrl, Alt** | **not tracked at all** |
| **Meta / Super** | **not tracked at all** |
| **F1–F12** | **dropped** — `if (sc >= 0x40) return;` |
| **arrows, Home/End/PgUp/PgDn, Insert/Delete** | **never even arrive** |
| **Right-Ctrl, Right-Alt** | **never even arrive** |

The last two rows are the important ones and they share one cause: those keys
are reported as a **`0xE0` prefix byte followed by the base scancode**, and
`ps2kbd_isr` has no `0xE0` case whatsoever. The prefix falls through to
`if (sc & 0x80) return;` — `0xE0` has the top bit set — so the prefix is
silently swallowed as if it were a break code, and then the *following* byte is
interpreted as an unprefixed make code.

That is worse than dropping the key: **right-Ctrl (`E0 1D`) currently types
nothing but leaves the driver having processed `1D`**, which in set 1 is
left-Ctrl's own code. Arrow-Up (`E0 48`) delivers `48`, which is ≥ 0x40 and so
returns — but Arrow-Left (`E0 4B`) delivers `4B`, also ≥0x40. The reason no
garbage has been observed is only that every extended key's base code happens to
land in the dropped `>= 0x40` range or on a modifier. That is luck, not design,
and it stops being luck the moment the `>= 0x40` cap is lifted to admit F-keys —
which is precisely what item 1 asks for. **Lifting the cap without adding `0xE0`
handling would inject phantom keystrokes.** The two changes are not independent
and must land together.

## 2. Why the ASCII ring cannot carry this

The ring is `volatile char g_ring[256]`. A chord is not a character: Alt-Tab,
Ctrl+Alt+T and Super+M have no ASCII encoding, and the compositor needs to know
that Alt is *held* while Tab arrives — state the byte stream cannot express.

Three options were considered:

1. **Escape sequences in the existing char ring** (`ESC [ A` for Up, terminal
   convention). Zero ABI change, and genuinely right for a terminal. Rejected as
   the *only* mechanism because it cannot express "Alt is down" as a state, and
   because it makes a literal ESC keypress ambiguous with a sequence prefix —
   a real defect for a shell that must handle both.
2. **Widen the existing ring and change SYS_INPUT_POLL's ABI.** Rejected: it
   breaks every consumer for the benefit of the few keys that need it.
3. **Keep the byte stream, add a parallel structured event ring.** Chosen.

Option 3 is chosen because the ABI surface is small and known: `SYS_INPUT_POLL`
has exactly **two** in-tree ring-3 consumers, `user/inputtest.c` and
`user/compositor.c`. Both keep working untouched — printable ASCII still flows
through NSI 46 exactly as before, so PRISM and the shell are unaffected — while
anything needing chords reads the new ring.

## 3. The structured event

```c
struct key_ev {          /* 4 bytes, fixed — a ring-3 ABI */
    uint8_t  code;       /* KEY_* keycode, driver-normalised, NOT a raw scancode */
    uint8_t  mods;       /* KMOD_* bitmask, state AT THE MOMENT OF THE EVENT */
    uint8_t  down;       /* 1 = make, 0 = break */
    uint8_t  ascii;      /* printable equivalent, or 0 — a convenience, not the identity */
};
```

`mods` is sampled **at the event**, not read later, because a consumer polling
afterwards would see the modifier already released — the classic chord race.

Keycodes are normalised so ring 3 never sees scancode-set trivia: left and right
Ctrl both report `KEY_CTRL` with `KMOD_CTRL`, and the L/R distinction lives in
`KMOD_CTRL_R` for anyone who wants it.

`SYS_KEY_POLL = 96`, verified free against `kernel/syscall/syscall.h` (max
shipped is 95, `SYS_RENAME`) rather than against §INV.14, per that invariant's
own instruction — it has been wrong before.

## 4. Break codes now matter

Today `if (sc & 0x80) return;` discards every break. A modifier tracker cannot
do that: without breaks, Ctrl latches down forever after one press. So breaks
are decoded for modifiers, and emitted as `down=0` events for everyone else.

This is the single riskiest part of the change, because a dropped or reordered
break leaves a phantom modifier held — and a phantom Ctrl turns ordinary typing
into control codes. §6's gate asserts the release edge explicitly for that
reason, not just the press.

## 5. Scope

This DDR delivers the *foundation* only: extended-scancode decode, modifier
tracking, F-keys/arrows, the event ring, NSI 96, and its gate. Super+M, Alt-Tab,
Ctrl+Alt+T and per-window routing are separate items that consume it, and each
gets its own gate. Batching their DDRs was the queue's instruction (§4.3); this
is the shared half they all failed on.

## 6. Gate

`smoke-modkeys`, a ring-3 probe driven by QMP `sendkey`, asserting:

- **A** plain `a` still arrives on NSI 46 — the old ABI is intact.
- **B** F1 arrives as `KEY_F1` with `ascii=0` — the `>= 0x40` cap is lifted.
- **C** Arrow-Up arrives as `KEY_UP`, exactly once — proving `0xE0` is consumed
  as a prefix and not re-interpreted as a make code. **This is the §1
  phantom-keystroke assertion**: a driver that swallowed the prefix would emit
  either nothing or a spurious second event, and both fail this arm.
- **D** Ctrl+C reports `KMOD_CTRL` set *in the same event* as `KEY_C`.
- **E** after Ctrl is released, the next key reports `mods == 0` — the §4
  release edge. Without this arm a latched-modifier regression passes.

---

# IMPLEMENTED

## 7. Result

`smoke-modkeys`: **PASS — all five arms**, kernel `c47a3cc7f930aa93`.

**Mutation-checked**, on the same principle as DDR-990 §4 — a gate that passes
with the feature removed is worse than no gate:

| kernel | change | outcome |
|---|---|---|
| `c47a3cc7f930aa93` | as shipped | all five arms pass |
| `22c585c5932775ef` | the `if (sc == 0xE0)` case deleted | **`MODKEYS FAIL: arm C — Arrow-Up never arrived (0xE0 prefix not decoded)`** |

The mutant fails with the precise diagnostic the arm was written to produce, and
the restore is byte-identical to the shipped kernel. Arm C is therefore load
bearing rather than decorative.

## 8. Two bugs found in my own work while building this

Recorded because both would have shipped silently and neither was caught by a
compiler:

1. **`SYS_YIELD` was written as 7; it is 3.** The probe's poll loop would have
   called an unrelated syscall every iteration. Caught by checking
   `syscall.h` instead of trusting recall — the same habit §3 applies to NSI 96,
   and the same one §INV.14 exists to enforce after it was itself wrong.
2. **The gate passed probe selection via `-append "probes=modkeys"`.** DDR-804
   passes probes through **fw_cfg**
   (`-fw_cfg name=opt/org.pradyos/probes,string=…`), not the kernel command
   line. The probe would never have been enabled. This one would have failed
   loudly rather than passed vacuously — the sentinel would simply never appear
   — but it is the same class of error: assuming a mechanism instead of reading
   it.

## 9. What is now unblocked

The four Group E items that could not be built on the DDR-703 driver now have
their foundation:

- **Super+M** sovereign toggle — needs `KMOD_META`, which did not exist.
- **Alt-Tab** — needs `KMOD_ALT` held *while* Tab arrives, which a byte stream
  cannot express.
- **Ctrl+Alt+T** — needs two simultaneous modifiers.
- **Per-window keyboard routing** — needs press/release edges, not just makes.

Each remains its own item with its own gate. This DDR deliberately does not
implement them: the shared half is what they all failed on, and bundling four
features behind one gate is how a green result stops meaning anything specific.
