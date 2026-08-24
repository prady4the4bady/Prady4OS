# DDR-993 — The modifier aggregate must be DERIVED, not assigned

**Status:** IMPLEMENTED
**Supersedes (in part):** DDR-991 §4 (`mods_set`), DDR-991 §6 (arm E's coverage claim)
**Gate:** `smoke-modkeys` — kernel arm, sentinel `PRADYOS_MODKEYS_PAIR_OK`
**Origin:** CodeRabbit review on PR #14. Found by review, not by a gate — which is
itself the finding, and §5 is about why.

---

## 1. The defect

DDR-991 tracked modifiers with

```c
static void mods_set(uint8_t bit, int down) {
    if (down) g_mods |= bit;
    else      g_mods &= (uint8_t)~bit;
}
```

and called it **twice** on each right-hand modifier edge — once for the side bit,
once for the aggregate:

```c
case 0x36: mods_set(KMOD_SHIFT_R, down); mods_set(KMOD_SHIFT, down);
```

The second call is unconditional, so the **break** code of one side clears the
aggregate regardless of the other side. The sequence is three keystrokes:

| step | scancode | intent | `KMOD_SHIFT` (DDR-991) | correct |
|---|---|---|---|---|
| 1 | `2A` | L-Shift down | 1 | 1 |
| 2 | `36` | R-Shift down | 1 | 1 |
| 3 | `B6` | R-Shift **up** | **0** | **1** — L-Shift is still held |

All four pairs are affected: Shift, Ctrl, Alt, Meta.

## 2. Why it is worse than a dropped Shift

For Shift the consequence is a wrong glyph — annoying, visible, self-correcting.
For **Ctrl, Alt and Meta it is a correctness hole**, because DDR-992 §2 keys the
chord suppression off exactly these bits:

```c
if (g_mods & (KMOD_CTRL | KMOD_ALT | KMOD_META))
    return;                       /* a chord is not text */
```

An aggregate that reads 0 while the key is physically held means the suppression
does not fire, so **a chord starts typing text again** — the precise regression
DDR-992 was written to prevent, reachable by pressing and releasing the other
Ctrl key. Super+M would flip the mode and then have the plain-'m' branch force it
straight back, which is DDR-992's own worked example.

Note the shape: two changes landed one commit apart (DDR-991 added the sides,
DDR-992 added the suppression), and the second silently made a latent bug in the
first into a security-relevant one. Neither commit's gate could see it.

## 3. The fix — an aggregate that cannot disagree with its sides

Only the eight **physical** keys carry state, in a kernel-private `g_side`. The
ABI's `KMOD_*` byte is **recomputed** from `g_side` on every modifier edge:

```c
static void mods_edge(uint8_t side_bit, int down) {
    if (down) g_side |= side_bit;
    else      g_side &= (uint8_t)~side_bit;

    uint8_t m = 0;
    if (g_side & (SIDE_SHIFT_L | SIDE_SHIFT_R)) m |= KMOD_SHIFT;
    ...
    g_mods = m;
}
```

This is deliberately not "add a conditional to the clear path". A guarded clear
keeps two writable representations of one fact and relies on every future edge
site to maintain the invariant between them; three sites already existed and a
fourth (Meta, `E0 5B` / `E0 5C`) had a different shape from the other three. A
derived aggregate has one representation, so the invariant holds by construction
and no future edge site can break it. The ring-3 ABI is byte-identical — `g_side`
never leaves the driver.

## 4. `code` must identify the key, `ascii` carries the glyph

Same review, same file, and it is the same class of bug: an identity that
depends on state which can change between two edges.

DDR-991 wrote the shifted glyph into **both** `code` and `ascii`:

```c
char c = (g_mods & KMOD_SHIFT) ? map_upper[base] : map_lower[base];
ev_push((uint8_t)c, (uint8_t)down, (uint8_t)c);
```

Shift+A therefore delivered `code='A'` on the make, and — because the Shift break
normally arrives before the letter's break — `code='a'` on the break. A consumer
tracking held keys by `code` leaks one entry per shifted keypress and never sees
the release. `code` is now taken from the **unshifted** column, which cannot
change between the two edges; `ascii` keeps the shifted glyph, which is what
`map_upper` is for. The header already claimed this ("`code` is NOT a raw
scancode … keys with a printable form report that character") — the code did not
implement it.

## 5. Why no existing arm caught either, and what that costs

DDR-991 §6 arm E was described as the arm that "matters most … a
latched-modifier regression passes every other arm here". That claim was true of
the regression it imagined (a modifier stuck **down**) and false of the one that
shipped (an aggregate stuck **up-when-held**). Arm E presses one Ctrl key, so
`g_side` never has two bits set and the buggy line and the correct line agree.
**The mutation check passed against a defect it could not express.** A gate is
only evidence about the sequences it can produce.

And it could not produce this one. QEMU's HMP `sendkey` emits a press and its
release as **one indivisible action** — there is no "hold". Two keys of one pair
held simultaneously is not a sequence `sendkey` can express, so the missing arm
was not merely unwritten, it was **unwritable** against DDR-991's shape, where
the only entry point was `ps2kbd_isr` reading port 0x60 itself.

Hence §6.

## 6. The decode is split from the port read

```c
void ps2kbd_feed(uint8_t sc);   /* the decode, byte in hand */
void ps2kbd_isr(void) { ps2kbd_feed(inb(0x60)); }
```

The IRQ path is otherwise unchanged — same decode, same rings, one extra direct
call. `ps2kbd_selftest()` then drives the exact sequences above as raw
scancodes, in ring 0, and returns 0 or the number of the step that failed. It
saves and restores **every** byte of decoder state (`g_side`, `g_mods`, `g_e0`,
both ring head/tail pairs), so a boot that runs it is indistinguishable from one
that does not; it runs before the probe is spawned and before any key is
injected, but restoring anyway means a later caller cannot be broken by it.

Twelve steps: the Shift pair (§1's table, plus that the **side** bit clears and
that the aggregate clears when the last side goes up), the Ctrl pair across the
`0xE0` prefix (§2's dangerous half), and §4's make/break identity.

## 7. Mutation check — REQUIRED, and this is the one that counts

§5 is the whole reason this section is not optional: DDR-991's arm E passed on
the broken kernel. Two mutants, each reverting exactly one of the two fixes:

**M1 — revert §3.** Restore DDR-991's `mods_set` and its two unconditional calls.
Expected: `ps2kbd_selftest` returns **3** (`KMOD_SHIFT` clear with L-Shift held),
gate fails on the `PRADYOS_MODKEYS_PAIR_OK` assertion.

**M2 — revert §4.** Restore `ev_push((uint8_t)c, down, (uint8_t)c)`.
Expected: `ps2kbd_selftest` returns **11** or **12** (make/break disagree).

A mutant that still passes means the arm is decoration and must be strengthened
before any green result from it is quoted — DDR-988 §9's vacuous-gate rule,
applied to the gate that just caught a bug the previous vacuous-gate rule missed.

Results: §8.

## 8. Results

Kernel hashes recorded per R1. The fixed kernel is `ff6bc6b1371f94c1`
(1,081,738 B, against the 1,572,864 B gate).

| kernel | hash | `ps2kbd_selftest` | `smoke-modkeys` |
|---|---|---|---|
| fixed | `ff6bc6b1371f94c1` | 0 (pass) | **PASS**, exit 0 |
| M1 (§3 reverted) | `b771cc4def3064c4` | **3** | **FAIL**, exit 2 |
| M2 (§4 reverted) | `d89d5a4a6fd3a0f0` | **11** | **FAIL**, exit 2 |

Both mutants failed at the predicted step — step 3 is "`KMOD_SHIFT` clear with
L-Shift still held", step 11 is "make reported `code='A'`, break reported
`code='a'`". Three distinct hashes, so no run measured a stale binary.

**The finding that matters is in the mutants' logs, not their exit codes.**
Both M1 and M2 still printed `PRADYOS_MODKEYS_OK`:

```
MODKEYS FAIL: kernel arm — ps2kbd_selftest step 3 (paired modifier / make-break identity)
PRADYOS_MODKEYS_WAIT
PRADYOS_MODKEYS_OK          <-- all six ring-3 arms passed on the broken kernel
```

So **every one of DDR-991's and DDR-992's ring-3 arms passes on a kernel with
either defect present.** That is §5 measured rather than argued, and it is why
the kernel arm is checked by its own `grep` rather than folded into the existing
`PRADYOS_MODKEYS_OK` assertion: had it been folded in, a kernel arm that
silently stopped running would leave the gate green on six arms while reporting
seven.

Restoring the fix reproduced hash `ff6bc6b1371f94c1` exactly — byte-identical to
the pre-mutation build, since the only other edit in that file is the §9
comment. The restore is verified, not assumed (§NON-NEGOTIABLE 16, applied to a
mutation revert).

## 9. An unrelated gap found while checking this one

`GLOBAL_FORBIDDEN` (`boot_test.sh:344`) is the list that makes a probe's failure
redden **any** gate it leaks into, not only the gate that declared it — the
mechanism `smoke-selftest`'s "foreign probe FAIL fails a gate that never declared
it" case exists to protect. Every older probe's sentinel is on it: `FSRM FAIL`,
`SURFDESTROY FAIL`, `CAPNET FAIL`, and twenty more.

Three recent ones were never added: **`FAT32MC FAIL` (DDR-973), `MODKEYS FAIL`
(DDR-991), `NETHAMMER FAIL` (DDR-990)**. Now appended (the list is append-only,
§NON-NEGOTIABLE 6). `SUPERKEY` needs no entry — that probe emits no `FAIL`
sentinel, asserting on its toggle lines instead.

The exposure was small, because all three probes are opt-in and only spawn in
their own gate. The point is the pattern: the safety net is only as good as the
last person who remembered to extend it, and three consecutive DDRs forgot.
**Adding a probe means adding its sentinel here.**

Note this edits `boot_test.sh`, so §11.4 applies — `smoke-selftest` re-run after
this change, not only after the earlier comment fix.

## 10. What is NOT claimed

- Not claimed that this closes any OPEN-* issue. It is an input-driver defect
  found by review, unrelated to the lwIP work in DDR-987/988/990.
- Not claimed that `smoke-modkeys` now covers the input path. It covers seven
  named arms. Key repeat, the `0xE1` Pause prefix, and the numeric keypad are
  all still undecoded and ungated.
- Not claimed that `smoke-nethammer` verifies which cpus its two instances ran
  on. It records no cpu ids; the same review asked for them. They were not
  added, because an NSI existing only for gate bookkeeping is the wrong trade
  and would be the weaker evidence anyway. DDR-990's mutant faults inside 1000
  iterations on a defect reachable only by two cpus in the lwIP core at once,
  which a serialised pair cannot produce — the fault establishes the
  concurrency. The gate comment now says this instead of leaving it implied.
- Not claimed that the single-source assumption for `g_side`/`g_mods`/`g_e0` is
  enforced. It holds because IRQ1 is PIC-only today and `ioapic_route()` is
  never called for it, so exactly one CPU runs the decoder — CodeRabbit
  confirmed this reading independently. It is an **implicit** invariant, and
  Group A's I/O APIC migration is precisely the change that would break it. A
  comment now says so at the declaration; that is a note, not a guard.
