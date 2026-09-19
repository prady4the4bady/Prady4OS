# DDR-1123 — `[apfreeze]` CARRIES THE PER-CPU TICK CENSUS

**Instrument change only. NO FIX, NO MECHANISM NAMED, OPEN-2 DOES NOT CLOSE.
No new clause, no new verdict, no new sentinel, no new gate — the set of
conditions that fire is IDENTICAL.**

Date: 2026-09-19 · design written before the code (§NON-NEGOTIABLE 5)

---

## §1 — WHY: the detector reports one CPU BY CONSTRUCTION

DDR-1122 measured it. `ap_freeze_probe()` latches a single victim:

```c
static int s_victim = -1;        /* the one CPU we sample, once chosen */
…
if (s_victim >= 0 && s_victim != (int)i)
    continue;
```

and **that is correct for what it was built to do**: spending all four shots on
one CPU is what makes `shot=1..4` at an identical RIP mean *spinning* rather
than *masked but running*. **`s_victim` IS KEPT.** Nothing here changes victim
selection, shot budget, NMI arming, or the relay.

What it cannot do is say **who else is frozen**, and the shard-7 capture of CI
34766468421 shows that is not hypothetical: the `[vblk]` line's census reads
`ticks[684,663,162,160]` **fourteen times**, i.e. CPU 2 *and* CPU 3 both pinned,
while `[apfreeze]` reports `cpu=2` alone and the block layer names `dest_cpu=3`.
The sibling capture on the same binary reads `ticks[754,733,164,730]` — one
frozen CPU — so the two shapes exist and are currently told apart only by
whether a `[vblk]` timeout happens to be in the same capture.

**That is the gap: the census exists, and it is in the WRONG LINE.** It rides on
`[vblk] compl wait timeout`, which prints only when a block completion strands.
A freeze with no block traffic produces no census at all, and every OPEN-2
capture has been read as one-frozen-CPU on the strength of one `[apfreeze]`
line.

---

## §2 — WHY THIS AND NOT DDR-1121's `key=`

DDR-1121 recorded-and-refused a `key=` echo inside `spin_lock_contended`:
*"a change to the hottest primitive in the kernel on the very path OPEN-2 lives
in, the cost DDR-1047 refused and DDR-1060 respected, designed off ONE
occurrence of this site."* **That refusal stands and is not revisited here**, and
DDR-1122 §6 adds that as literally worded it is also redundant.

Every clause of that objection fails to reach this change, which is why it is
built rather than recorded:

| DDR-1121's objection to `key=` | applies here? |
|---|---|
| on the hottest primitive in the kernel | **No** — `ap_freeze_probe`'s print arm runs *only after a CPU has already frozen* |
| an always-on cost (DDR-1047) | **No** — a healthy boot prints none of it, the same failure-path-only discipline DDR-1047 and DDR-977 §6 set |
| could perturb the timing it measures (DDR-1010) | **No** — the boot is already over as an experiment by the time this runs |
| designed off ONE occurrence of a rare site | **No** — it closes a **structural** limitation that has shaped the reading of *every* OPEN-2 capture ever taken |

---

## §3 — WHAT SHIPS

One block, inside the existing `console_line_lock()` region of the print arm,
immediately before the line's `\r\n`:

```
 ticks[0=684,1=663,2=162,3=160]
```

Three deliberate choices:

1. **Present CPUs only, each tagged with its index.** `[vblk]`'s census is
   `for (_c = 0; _c < 4u; _c++)` — hardcoded four, positional. `PERCPU_MAX` is
   **16**, so that form is silently wrong above `-smp 4` and gives a reader
   nothing to check an index against. Tagging costs one `kputdec` and one
   character per CPU and removes both problems. **`[vblk]`'s line is NOT
   changed** — it is correct at every width this project runs, and editing a
   line that four DDRs have quoted verbatim would cost more than it buys.
2. **`cp->ticks` read per CPU through `percpu_get(c)`**, never `this_cpu()`.
   `this_cpu()` reads `%gs:0`, and DDR-1010 caught a broken SWAPGS discipline as
   one of OPEN-2's own producers; `percpu_get` is an index into the array and
   needs no GS at all. This is the same reasoning DDR-1060 §3 used to keep
   `lock_stat` per-lock rather than per-CPU.
3. **Nothing is judged.** No threshold, no "frozen" verdict, no clause. The
   numbers are printed and the reader compares them, exactly as with `[vblk]`'s.
   A verdict would need a window to compare against, and the only window
   available here is the one `s_victim` already consumed.

### §3.1 — A pre-existing exposure that is NOT changed and NOT fixed

This line is built from ~25 separate `kputs`/`kputhex`/`kputdec` calls inside
`console_line_lock()`, and `console.c:315` states the limit in its own words:
that lock *"excludes only OTHER holders of `g_line_lock`, and the busiest
printer in this system — `kwrite`, the ring-3 write path — …"*. So `[apfreeze]`
is splice-prone (DDR-1055's class) **today**, and adding fields widens that
window slightly.

**Not fixed here, with the reason measured rather than waved:** the single-write
remedy is `kline`, `KLINE_MAX` is **256**, and this line already exceeds that
before any addition (~25 fields including four 18-character `bt=` values).
Converting it would require raising `KLINE_MAX`, which **DDR-1118 refused** —
24 call sites share that constant and one of them is in `schedule_locked`, the
hottest frame in the kernel. Recorded as unchanged in kind, not as absent.

---

## §4 — PROOF, AND THE VACUITY CHECK DONE FIRST

**The obvious arm is vacuous** (the twenty-first time this is caught in design
text): *"force a fire and check `ticks[` appears"* proves only that the field
prints. A plausible wrong implementation — reading `this_cpu()->ticks`, or
`pc->ticks`, once per index — prints a well-formed array of the right length
with the right punctuation and **is wrong in exactly the way that matters**,
because every entry would be the same number and the instrument would report a
census that cannot distinguish one frozen CPU from four.

**So the discriminating property is that the entries DIFFER per CPU**, and the
proof is built on that:

- **M0 (forced control).** The triggering condition cannot be manufactured in
  product (DDR-1105 §8), so the print arm is forced on a **healthy** `-smp 4`
  boot. A correct census then shows **four distinct, climbing** tick counts.
- **M1 (mutant).** The census reads `pc->ticks` — the victim's own counter — for
  every index, instead of `percpu_get(c)->ticks`. This is the wrong
  implementation a reviewer would most plausibly write, it type-checks, it
  compiles warning-clean, and it prints **four identical values**. M1 passes the
  vacuous arm and fails the real one, which is the whole claim.

Both on recorded hashes, one line apart (DDR-1042: never mutate two things).

**The negative half** is a healthy regression set showing **zero** `[apfreeze]`
lines and therefore zero census output, because a healthy boot must pay nothing:
`smoke-shell`, `smoke-smp`, `smoke-smppreempt`, `smoke-rqstress`,
`smoke-blk-integrity`, with the kernel hash pinned **before and after** every
gate (DDR-1060 §9's void-campaign rule).

**NO GATE ARM**, and the reason is DDR-1105 §8's unchanged: the triggering
condition cannot be manufactured in product, and an arm asserting the *absence*
of a rare intermittent is unfalsifiable at any N this project can afford — the
shape DDR-1082 costed and refused. **179 gates unchanged.**

---

## §5 — NOT CLAIMED

- **NO FIX. NO MECHANISM NAMED. OPEN-2 DOES NOT CLOSE**, and no open issue moves
  (OPEN-1 / OPEN-2 / OPEN-12 / OPEN-13 untouched). What changes is what the
  **next** occurrence can say, not anything about a past one.
- **NO DEFECT FOUND IN `idt.c` AND NONE ALLEGED.** `s_victim`, the shot budget,
  the NMI arming and the relay are all correct for what they were built to do and
  are all untouched; what DDR-1122 corrected was a **reading**, not the code.
- **NO DEFECT ALLEGED IN `virtio_blk.c`**, whose census is correct at every width
  this project runs and is deliberately left alone.
- **NO RATE.** One capture with two frozen CPUs; its sibling on the same binary
  has one.
- **NO new clause, NO new verdict, NO new sentinel** (`GLOBAL_FORBIDDEN` stays
  **77**), **NO new gate** (179), no new probe ELF (79).
- **NOT EXONERATED IN ADVANCE** (DDR-1042): this edits `idt.c`, which is
  load-bearing for DDR-981 / DDR-1006 / DDR-1010 and for the still-open OPEN-2,
  so if the OPEN-2 signature moves, this commit is a candidate and *"the diff is
  elsewhere"* is not an argument.
- **§3.1's splice exposure is NOT fixed** and is recorded as unchanged in kind.
