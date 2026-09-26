# DDR-1086 — the checklist row my own previous commit falsified, and a checker the measurement refuses

**Status:** DOCS-ONLY correction + a checker measured and NOT built
**Date:** 2026-09-07
**Branch:** `dev/phase1-seyp3n`

---

## 1. DDR-1084 named a pattern and then committed an instance of it

DDR-1084 §1's finding, one commit ago:

> **a DDR that retires a blocker does not, by default, revisit the row the
> blocker was holding** … because that session was working on the SUBSYSTEM,
> not on the ROW.

It proposed a cheap substitute in place of a checker: *"when a DDR retires a
blocker it names the rows that blocker was holding."* DDR-1084 **did** that — it
rewrote `CLAUDE.md`'s Group F `ACTION_SEND_IPC` row in the same commit.

**And `docs/PRE_LAUNCH_CHECKLIST.md` §4.1 still reads, today:**

> **`ACTION_SEND_IPC` remains unwired, and this row is now accurate about that
> being the exception.**

That sentence was true when DDR-1066 wrote it and false from `a6bc7b0` onward.
`user/actionipctest.c` submits the type, the probe obeys the verdict, and
`smoke-sendipc` gates both arms.

**So the substitute is insufficient as stated, and this is the evidence.** It
says "names the rows" — singular document implied. There are at least **two**
documents carrying the same claim, and following the rule against the one I was
editing left the other false. The corrected form: **a DDR that retires a blocker
names the rows in EVERY document that records it** — for this project that is at
minimum `CLAUDE.md` and `docs/PRE_LAUNCH_CHECKLIST.md`.

That is not a stronger rule so much as a more honest one. It is still a
discipline and not a control, and §4 below is why no control replaces it.

### 1.1 The counter-example matters, because the discipline is not uniformly absent

§4.11 (`lock_stat` cannot see `mnt_lock`) **was** correctly updated, by DDR-1060,
in the commit that fixed it. The difference is exactly DDR-1071 §4's reading:
§4.11's row is *about the instrument DDR-1060 was changing*, so it was adjacent
work; §4.1's row is about the *action path* while DDR-1084 was working on the
*probe*. Correction as a side effect of adjacency, again — which is why it lands
sometimes and not others, and why it is not a process.

---

## 2. §4.3 needs nuance, not deletion

§4.3 says `ACTION_SEND_IPC` *"auto-approves in sovereign mode with nothing to act
on it."* Half of that is still exactly true and must not be swept away with §4.1:

* **auto-approves in sovereign mode** — TRUE and unchanged.
  `aether_action_forces_pending()` was deliberately not touched by DDR-1084;
  whether the type belongs in that list is a DDR-842 S4 policy decision recorded
  for the operator.
* **nothing to act on it** — now false *for a probe* (`actionipctest.c` acts) and
  still true *for the shipped agent*: `user/agent_base.c` submits
  `ACTION_WRITE_FILE` and nothing else, so no shipped agent acts on an approved
  `SEND_IPC`.

Deleting the row would over-claim; leaving it would under-state. It is split.

---

## 3. §6 carries a stale DDR free range, and is a FOURTH carrier nothing names

`docs/PRE_LAUNCH_CHECKLIST.md:1365` reads **`DDR-1083+`**. Occupied since
`4a75699`; 1084 and 1085 have landed since.

**Set to `DDR-1087+`, not `1086+`** — 1086 is this correction itself, so the
range advances past the DDR that fixes it. Writing `1086+` would have re-created
the identical one-off staleness *inside the edit that repairs it*; caught before
commit, and recorded because the near-miss is the same shape as the defect.

**The repair for the recurrence is in `CLAUDE.md`, not here:** its "update both"
warning now reads "all three, **and a fourth carrier outside this file**", naming
this checklist. A warning that under-counts its own carriers is what let three
correct sites coexist with one wrong one.

`CLAUDE.md`'s own warning under §CURRENT BUILD STATE says:

> This file carries the free range in TWO places (§INV.4 and here) and they have
> disagreed before — update both.

and §ORIENTATION says *"§INV.4 and §CURRENT BUILD STATE carry it too; all three
must be updated together."* **All three CLAUDE.md sites are correct at
`DDR-1086+` — measured.** The checklist is a **fourth** carrier that neither
warning names, which is why updating "all three" left it behind.

### 3.1 Severity, stated rather than dramatised

The first reading is that this causes the two-files-one-number collision
`DDR-NUMBERING-MAP-2026-08.md` exists to record. **It does not, provided the
procedure is followed**, and the procedure is designed for exactly this:

* §NON-NEGOTIABLE 8 requires `ls docs/ddr/ docs/decisions/ | grep DDR-<N>` to
  return empty in **both** directories *before* allocating.
* §ORIENTATION says outright: *"allocate by §NON-NEGOTIABLE 8's command, not from
  this line."*

So a stale range costs a wasted lookup and a moment's doubt, not a collision —
**unless** the session skips the `ls`, which is the thing the non-negotiable
exists to stop. Worth fixing; not worth calling a near-miss.

---

## 4. A mechanical checker: MEASURED, and the measurement refuses it

This looked like the first document-staleness signal that is **mechanical**
rather than semantic, and therefore the first buildable one after three
refusals (DDR-1071 §5, DDR-1072 §2, DDR-1081 §3). The rule writes itself: *a
stated `DDR-N+` free range must not name an occupied DDR.*

**Measured before writing a line of it** — every `DDR-<N>+` in the tracked
documents, against `ls docs/ddr/ docs/decisions/`:

| stated range | site(s) | occupied? | correct? |
|---|---|---|---|
| `DDR-1086+` | `CLAUDE.md` ×3 (§INV.4, §CURRENT BUILD STATE, §ORIENTATION) | no | ✅ live |
| `DDR-1083+` | `PRE_LAUNCH_CHECKLIST.md:1365` | **YES** | ❌ **the defect** |
| `DDR-1065+` / `1064+` / `1063+` | `CLAUDE.md`, each written `(prior: …)` | YES | ✅ **historical, correct** |
| `DDR-1034+`/`1060+`/`1066+`/`1067+`/`1068+`/`1069+` | `SESSION_HANDOFF.md`, one per checkpoint | YES | ✅ **historical, correct** |

**Ten of the eleven stated ranges name an occupied number, and NINE OF THOSE TEN
ARE CORRECT.** A naive checker reddens on nine correct records to catch one
defect — a 9/10 false-positive rate, on a file (`SESSION_HANDOFF.md`) whose whole
purpose is to be an append-only historical log where every past checkpoint's
free range is *supposed* to be occupied by now.

**And this is not a new shape — it is the one §6 already documents about itself.**
DDR-1063's `ci-docstate-check` has the identical limitation, recorded in this
same section: its §5.1b.1 pairing (`1,175,946 / 396,918`) names the
pre-post-quantum kernel and is **correct as written**, because the surrounding
prose says those are deliberately historical. *"`ci-docstate-check` cannot tell a
deliberately historical pair from a live-state one; here a human annotation
does."* The same sentence applies here word for word with "pair" replaced by
"range".

**Narrowings considered and refused:**

* *Only check lines lacking "prior"/"was"* — pins a **spelling**, which is
  DDR-1077's stated reason for pinning `->cr3` writers rather than `CLONE_VM`.
  A future historical note worded differently reddens.
* *Only check named sites by path and anchor* — a rule about today's document
  layout rather than about the claim, which is precisely what DDR-1071 §5
  refused for the strict-tier variant.
* *Assert the MAXIMUM stated range is unoccupied* — passes today and would have
  passed on the defect too, since `CLAUDE.md`'s correct `1086` is the maximum.
  It cannot see a stale *lower* live-state site, which is the entire failure.

**NOT BUILT.** Fourth refusal in this class, and the first killed by a
**measurement** rather than by reasoning about semantics — which is the part
worth carrying: the signal here really is mechanical, and it still does not
support a check, because the corpus legitimately contains nine instances of the
pattern the check would flag.

---

## 5. What is accurate, stated because an audit that only reports errors is not an audit

* **§6's `kernel.bin` pair is CURRENT** — `1,307,018 B` / `265,846 B` headroom
  matches the tree exactly. It is current **by luck**, not by discipline:
  DDR-1084 set that size and DDR-1085 left `kernel.bin` byte-count unchanged, so
  the pair DDR-1081 §5 had to correct has not drifted since. A third observation
  of the same limitation, this time in the passing direction.
* **§6's gate count (178), excluded count (6) and NSI row are correct.**
* **§4.11 is correctly updated** (§1.1).
* **§4.4, §4.14, §4.15, §4.16 are all still true as written** — the `invlpg` is
  still uncovered, I/O APIC stage D and KASLR are still deferred on their stated
  blockers, and privacy mode still refuses I/O without tearing the connection
  down.

---

## 6. NOT CLAIMED

* **No code change.** `kernel.bin` is untouched, so the size/headroom pair and
  `ci-docstate-check` are unaffected; 178 gates unchanged; `GLOBAL_FORBIDDEN` 76
  unchanged.
* **No gate was run for this DDR**, locally or in CI. What was measured is file
  existence under `docs/ddr/` and `docs/decisions/`, the stated ranges by grep,
  and the four checklist rows read in full.
* **No checker is built** (§4), and the refusal is on a measurement, not a
  preference.
* **No policy change** — `aether_action_forces_pending()` is untouched and §2's
  auto-approval half stands exactly as recorded.
* **No open issue moves** (OPEN-1/2/12/13 untouched); this is not an apfreeze and
  not OPEN-2.
* **The `SESSION_HANDOFF.md` historical ranges are NOT "fixed"** — they are
  correct as append-only records, and normalising them would destroy the history,
  the same mistake §6 warns about for the §5.1b.1 kernel pair.
