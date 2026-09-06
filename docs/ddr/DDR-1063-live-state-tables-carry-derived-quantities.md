# DDR-1063 — A LIVE-STATE TABLE CARRIED A DERIVED QUANTITY, AND ITS INPUT MOVED WITHOUT IT

**Status:** FIXED (both documents) + GATED (`ci-docstate-check`, hygiene + CI) + M0/M1/M2/M3
**Date:** 2026-09-05
**Tree:** `dev/phase1-seyp3n`, on `0089e08`
**Kernel:** UNCHANGED — `c33afa79f60abdcb`, 1,278,346 B. This change touches no
kernel source. The gate is a static text check.

---

## 1. The defect

`CLAUDE.md` §CURRENT BUILD STATE read:

> **`kernel.bin`**: **1,278,346 B** against the 1,572,864 B size gate — 396,918 B
> of headroom

`1,572,864 − 1,278,346 = 294,518`. The stated headroom is `1,572,864 − 1,175,946`
— the headroom of the **pre-post-quantum** kernel. The SIZE was updated as ML-DSA
landed (DDR-1052 → 1054 → 1057 → 1058) and the **subtraction beside it was not**,
so the file **overstated the remaining budget by 102,400 B**.

`docs/PRE_LAUNCH_CHECKLIST.md` §6 carried the same pair one revision further
back: size *and* headroom both from `32cb8ad`, self-consistent but stale, so its
gate count (173/7 excluded) and DDR free range (1050+) were stale too.

**DDR-1058 computed it correctly in its own text** — "294,518 B of kernel.bin
headroom". So the project held both numbers simultaneously and the wrong one was
in the file §MANDATORY FIRST ACTIONS tells every session to read, and §INV
explicitly tells it to trust *without re-deriving*.

## 2. Why it matters, and why "it's only a doc" is wrong here

Headroom is not decoration: it is the number a session uses to decide whether a
subsystem **fits before building it**. DDR-1058 reasoned about it explicitly when
sizing ML-DSA; DDR-1051 and DDR-1059 both size candidate work against it.

And the consequence of getting it wrong is not a compile error. §INV.18's binding
quantity is file **+ BSS** against the 2 MiB `PT_HI` span read through a 48-chunk
stage-2 window. Overrunning that is a **boot failure**, discovered at QEMU time,
after the work is written.

102,400 B is roughly the whole ML-DSA landing (§4). A session planning against
the wrong figure has one full subsystem of phantom budget.

## 3. Why every existing gate stayed green

`Makefile:697` **does** enforce the ceiling:

```
@test "$$(wc -c < $(KERNEL_BIN))" -le 1572864 || { echo "kernel.bin exceeds 1.5 MiB ..."; exit 1; }
```

That gate checks the **binary** against the ceiling. It says nothing whatever
about what the **documentation** claims about that binary. The doc could state any
number at all and all 176 gates stay green.

This is not the DDR-1046 shape (a control that could not see the case it exists
for). It is the case with **no control at all**: nothing in the tree has ever
compared a documented quantity to anything. That is the gap worth carrying —
`hygiene_check.sh`'s six checks cover shards, probe rodata, `_start` alignment,
the resize checker, and apt; **not one of them reads a claim in a document.**

## 4. What was measured

| quantity | stated | actual | error |
|---|---|---|---|
| `kernel.bin` headroom (CLAUDE.md) | 396,918 B | **294,518 B** | **+102,400 B** |
| gates assigned (checklist §6) | 173 | **176** | −3 |
| gates excluded (checklist §6) | 7 | **6** | +1 |
| DDR free range (checklist §6) | 1050+ | **1063+** | 13 numbers |

Gate figures re-measured with `make ci-shard-check` at `0089e08`:
`176 gates assigned across 10 shards, 6 excluded with reasons`.
`kernel.bin` measured 1,278,346 B, hash `c33afa79f60abdcb`.

The checklist's own closing line already required this:
*"Update this file in the same commit as any change to what it records."*
DDR-1061 changed the exclusion set and did not. **That is my process gap from the
previous session, recorded as mine.**

## 5. The fix, and the check that was DELIBERATELY NOT built

### 5.1 Rejected: assert the stated size equals the actual build

The obvious check — `grep` the size out of `CLAUDE.md`, compare to
`wc -c build/kernel.bin` — is **wrong, and would have been actively harmful.**

It fails on *every* commit that changes the kernel before the doc is updated,
which is the normal state of a working tree mid-task. A check that reddens on
correct in-progress work does not get obeyed; it gets `|| true`'d or removed.
That is the DDR-1045 failure mode with the roles reversed — there a check was
built on a guess about an environment; here it would be built on a workflow
assumption that is simply false.

It also demands something the project has never promised: the docs are updated
**per landing**, not per build.

### 5.2 Shipped: internal consistency, which cannot false-positive

`tools/ci/docstate_check.py` asserts, for every live-state pairing of a size and
a headroom against the 1.5 MiB ceiling:

```
stated_size + stated_headroom == CEILING
```

This makes **no claim about currency**. It catches exactly the observed failure
mode — *one half of a pair updated, the other carried forward* — and it is
impossible for it to fire on a doc that is merely out of date, because a stale
pair updated together still sums correctly (which is precisely why checklist §6
was self-consistent while wrong, and why this check is a floor, not a ceiling;
§8 states that limit).

The ceiling is **read from `Makefile:697`, not hardcoded**, so the check cannot
drift away from the gate it describes. A hardcoded 1572864 would keep passing
after a future window raise while describing a bound that no longer exists.

### 5.3 The vacuity trap, handled first

A regex check over prose that matches **nothing** passes trivially and forever.
That is the dead-arm class — twelfth-plus instance in this project, and the
reason it is designed for before the code rather than discovered after.

So `docstate_check.py` **counts what it found and FAILS on zero**:

```
docstate: FAIL — no size/headroom pairing found in <file>
```

It also prints every pairing it checked with the file and line, so a reader can
confirm the check looked at the lines they think it looked at. `rc=0` with a
silent body would be worth nothing (DDR-1041's lesson, read back rather than
assumed).

## 6. Proof — two-sided, on real text not synthetic

**Three arms, landing on three DIFFERENT failure modes.** Counts below are
measured, not predicted: this DDR's draft said the check would find **2**
pairings and it finds **3**, because §5.1b.1 fact 2 carries a dated pre-work
pairing as well. That third one **passes**, and it should: it is internally
consistent for its own date. That is the design working — the check tests
consistency, not currency (§5.2) — and the corrected number is recorded here
rather than the predicted one.

**M1 is not a synthetic mutant: it is the pre-fix `CLAUDE.md`**, the same
construction DDR-1046 used. Restoring the `396,918 B` line:

```
docstate: CLAUDE.md:394 size=1278346 headroom=396918 sum=1675264 != 1572864 (off by +102400)
docstate: docs/PRE_LAUNCH_CHECKLIST.md:595 size=1175946 headroom=396918 sum=1572864 OK
docstate: docs/PRE_LAUNCH_CHECKLIST.md:931 size=1278346 headroom=294518 sum=1572864 OK
docstate: FAIL - 1 inconsistent pairing(s) of 3 checked
rc=1
```

It names the file, the line and the exact error, and the two correct pairings in
the same run stay green — so the arm discriminates rather than just reddening.

**M0** — the same check on the **fixed** tree: `rc=0`,
`docstate: OK - 3 pairing(s) checked, 0 inconsistent`. Reverting M1 returns
`rc=0` exactly.

**M2 (vacuity arm)** — drift the wording past `PAIR_RE` in both inputs so nothing
matches. The check must NOT pass:

```
docstate: FAIL - no size/headroom pairing found (checked 2 file(s))
rc=1
```

M2 is the load-bearing one. Without it, "the check is wired up" and "the check
matches nothing" are indistinguishable, and any future rewording of those lines
could silently retire the check while it kept reporting success.

**M3 (ceiling drift)** — quote a `2,097,152 B` gate in prose while the Makefile
enforces 1,572,864:

```
docstate: CLAUDE.md:394 ceiling=2097152 != Makefile 1572864
docstate: FAIL - 1 inconsistent pairing(s) of 3 checked
docstate:        1 quote a ceiling the Makefile does not enforce.
rc=1
```

M3 exists because that guard would otherwise have been **unproven code shipped in
a checker whose whole subject is unverified claims**. It also found a defect in
this DDR's own first draft: the failure path printed *"Recompute headroom from the
size"* for a **ceiling** drift, which is the wrong remedy and would have sent a
reader to the wrong line. The two defects now report separately.

## 7. Also corrected in this change

- `CLAUDE.md` §CURRENT BUILD STATE — headroom 396,918 → **294,518 B**, with the
  cause recorded inline and the rule stated: *recompute headroom from the size in
  the same edit; never carry it forward.*
- `docs/PRE_LAUNCH_CHECKLIST.md` §5.4 — `smoke-sfs-btree-smp4` row removed,
  7 → **6 excluded**, DDR-1061's registration recorded, **and the clause "waiting
  on promotion evidence, not on work" is retracted**: it was never part of the
  exclusion's stated condition, and DDR-1061 §2 shows no reachable N could have
  supplied it.
- `docs/PRE_LAUNCH_CHECKLIST.md` §6 — re-measured at `0089e08`: 176 gates,
  6 excluded, DDR free range 1063+, `kernel.bin` 1,278,346 B / 294,518 B headroom
  with its hash, plus an explicit *re-derive, do not carry forward* instruction
  beside the existing *re-measure rather than increment*.
- `docs/PRE_LAUNCH_CHECKLIST.md` §5.1b.1 fact 2 — left as written (it is a
  **dated pre-work assessment** and its prediction was borne out), annotated with
  what the work actually cost: 102,400 B, i.e. **26% of the headroom it was
  measuring**. "Size is not the constraint" was a claim about ML-DSA-44
  specifically and must not be reused as a general claim about what still fits.

- Wired in **two** places, deliberately: `tools/ci/hygiene_check.sh`
  (**ALL SIX → ALL SEVEN**, and `CLAUDE.md`'s hygiene item updated to match — that
  item has drifted from the script before, which is why the script is
  authoritative) and `.github/workflows/ci.yml`'s `shard-check` job, which
  installs no toolchain and needs none: `python3`, the `Makefile` for the ceiling,
  and two checked-in documents.
- `Makefile` `.PHONY` also gained `ci-runnerenv-selftest`, which DDR-1048 added to
  the hygiene script and to the target list but never to `.PHONY`. Harmless today
  (no file bears that name) and corrected while adjacent.

`docs/build_status.md` and `docs/BUILD_TRACKER.md` are **NOT** touched and are
**NOT** in the check's input set. They are dated append-only logs: each entry was
correct at its own commit and rewriting history to match today's numbers would
destroy the record. The check's scope is the two **live-state** tables, and the
distinction between a live table and a historical log is the whole reason the
scope is a list of two files rather than a repo-wide sweep.

## 7b. FOUR MORE STALE ENTRIES IN THE SAME DOCUMENT — the thesis, confirmed

Sweeping `docs/PRE_LAUNCH_CHECKLIST.md` for the *same class* of defect (an entry
not updated when the work it describes landed) found **three more**, in the
document whose own stated purpose is *"every deferred/open item, one document"*
and whose closing line already says *"Update this file in the same commit as any
change to what it records."*

1. **DDR-1062 was absent entirely** — OPEN-2's **first CI-side rate bound** (42
   suites, zero occurrences, 95% upper bound 6.9%, DDR-1009's 25% refuted at
   `p = 5.7e-6`). That is the single most important new fact about the project's
   top open defect, and the one-document reference did not carry it.
2. **DDR-1056 was absent entirely** — it *fixed* the `smoke-actiondel` splice
   class, while the Section 2 row still read as though only a mechanism had been
   named and the ring-3 fix were pending.
3. **§4.8 said the ghost-window repair was "named, not built"** — it is built and
   gated. `mouse_inject.sh:131` consumes `PRADYOS_WM_GONE` and prints
   `[inject] target gone title=… — not clicking`; `smoke-ghostclick` gates it.
   **Checked in the tree, not assumed from the DDR text.**

4. **§5.3's gate inventory was wrong on four of its seven Group A claims** — and
   §5.3 is the section that *opens* by saying it was measured by grepping the
   Makefile "because declaring something unbuilt without grepping has been wrong
   four times in this project." Re-measured at `c8b041b`: `smoke-smep` **EXISTS**
   (DDR-1040), `smoke-smap` **EXISTS** and was not even listed, `smoke-mce`
   **EXISTS** under a name the row got wrong (`smoke-mc` has never existed), and
   `smoke-wx` — the fourth name — **has never existed either**; the real gate is
   `smoke-wxkernel`, the same wrong name DDR-1040 had already found in
   CLAUDE.md's own Group A row. Two whole gates were missing from the section
   (`smoke-shake`, `smoke-mldsa` — the post-quantum set §PHASE 3 makes mandatory
   v1 scope), and **Group D contradicted itself inside one section**, listing
   `smoke-poll` as MISSING three paragraphs after saying it EXISTS.

All four are corrected. This is the same failure mode as §1 with different
quantities: a live-state document is only as good as the discipline that updates
it, and **nothing mechanical was checking any of it.** `ci-docstate-check` closes
exactly one of these — the size/headroom pair — and §8 says plainly that it does
not close the rest. Recorded here so the size of the residual is visible rather
than implied: **five stale items were found in one sweep of one document**, and
the check catches one shape of one of them.

**A gate inventory is mechanically checkable and is NOT checked here**, which is
the largest single piece of the residual. `ci-shard-check` already knows every
`smoke-*` target in the Makefile and every one in the shard manifest, so a check
that a *document's* claimed EXISTS/MISSING inventory matches the Makefile is
buildable on machinery that already exists. It is not built in this change
because the prose form is free (each claim is a bare target name in running
text, not a table this session should reshape days from a release), and building
it means first choosing a machine-readable form for those claims — a real design
decision, not a line of code. **Named as buildable-and-not-built rather than
left implicit**, so the next session inherits the option instead of re-deriving
the gap.

## 8. NOT CLAIMED

- **This does not make the documented numbers correct** — it makes an
  *internally inconsistent* pair impossible. A pair that is stale but
  self-consistent still passes, which is exactly what checklist §6 was. Currency
  is still a human discipline, and §NON-NEGOTIABLE 11 is still the rule that
  carries it.
- **No kernel defect is fixed and no open issue moves.** OPEN-1, OPEN-2, OPEN-12,
  OPEN-13 are untouched. `kernel.bin` is bit-identical.
- **The check covers one derived quantity, not all of them.** Gate counts, NSI
  max and the DDR free range are equally derived and equally capable of going
  stale; they are not checked here because each needs a different oracle
  (`ci-shard-check` already re-measures gates on demand, `syscall.h` is the NSI
  authority). Extending it is cheap; asserting it is complete would be false.
- **The 102,400 B error's downstream effect was not measured.** No decision is
  known to have been made on the wrong figure — DDR-1058 used the right one. The
  claim is that a wrong number sat in the trusted file, not that it caused harm.
