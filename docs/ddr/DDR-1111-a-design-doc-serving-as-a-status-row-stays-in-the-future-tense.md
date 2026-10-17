# DDR-1111 — A design document serving as a status row stays in the future tense: §1.5 denies the instrument this session has been using as evidence

**Status:** ASSESSMENT + CORRECTION — docs-only, no code change, no gate,
**no defect found in any code and none alleged**
**Date:** 2026-09-13
**Written:** in the CI-wait window on `704b2a5`, whose two suites are in flight
with their shards queued. Docs-only by choice: a kernel change stacked under a
running suite cannot be attributed (DDR-1107's own rule).

---

## §1 — The row, and what it says

`docs/PRE_LAUNCH_CHECKLIST.md` §1.5, *"CI efficiency refactor: CONFIRMED SAFE,
with two hazards that must be handled"*, reads in the **future tense
throughout**:

> "Queued as item 5 (after this checklist and RUN_EXPERIMENT); **this is the
> confirmation, not the change.**"
>
> "**Which is why the refactor SHOULD make that structural rather than weaken
> it:** have the build job publish `sha256sum build/kernel.bin`, and have each
> shard **assert the hash it downloaded matches** before running a single gate.
> **Today** 'all 10 shards ran the same binary' is **inferred** from them
> compiling the same source; **afterwards it WOULD be checked**."

**Every clause of that shipped, and it shipped in DDR-1035 on 2026-09-01.**

The row is stale in the **safe** direction — it under-reports finished work — but
it sits in **SECTION 1, OPERATOR DECISIONS REQUIRED BEFORE USER TESTING**, so a
reader of the one section headed *decisions a person must make* concludes a CI
refactor is still outstanding.

**AND THE ROW DENIES AN INSTRUMENT THIS SESSION HAS LEANED ON TWICE.** DDR-1108
§11.1 established the CI verdict's binary identity by reading the build job's
published digest; forty minutes ago the same reading was taken again for
`704b2a5`. Both times the claim rested on exactly the mechanism §1.5 calls
"inferred, not checked". The row does not merely under-report — **it contradicts
the evidence standard the current work is being held to.**

---

## §2 — Measured clause by clause, in `ci.yml`, not inferred

| §1.5 clause | State in the tree | Where |
|---|---|---|
| "**10 identical kernel compiles per CI run**, plus 10 apt installs and 10 rustup installs" | **FALSE.** No shard runs `make image`, `make musl`, `make lwip` or `make ci-probe-rodata-check`, and none runs `rustup` at all. (Two greps match inside that job and **both are comments** — the submodule note and the `# No rustup` note.) | `ci.yml:175-260` |
| "have the build job publish `sha256sum build/kernel.bin`" | **BUILT**, and it `cat`s it so the digest lands in the job log | `ci.yml:155-158` |
| "have each shard assert the hash it downloaded matches **before running a single gate**" | **BUILT** | `ci.yml:214-215` |
| (not asked for) | **AND AGAIN AFTER**, under `if: always() && steps.fetch.outcome == 'success'`, so a red shard still reports whether it ran the right binary | `ci.yml:254-256` |
| **Hazard 1** — "make is mtime-driven … download-artifact does not preserve mtimes" | **HANDLED.** `find build -exec touch {} +`, under a comment that restates the hazard in the row's own words | `ci.yml:211-212` |
| **Hazard 2** — "`smoke-selftest` must stay per-shard … Do not fold it into the build job" | **HONOURED**, and the workflow says so verbatim: *"DELIBERATELY still a setup step in EVERY shard rather than folded into the build job above."* `smoke-selftest` appears exactly once in the file, in the shard job | `ci.yml:222-223` |
| "**Unchanged, as instructed:** `fail-fast: false`" | **TRUE**, on both matrices | `ci.yml:179`, `:287` |
| "Toolchain caching (apt + rustup) … is the safer half to do first" | **SPLIT.** rustup is not cached, it is **removed** — strictly better, and the build job's comment says so: *"a removal, not a cache."* **apt is neither cached nor removed** | §4 |

So seven of eight clauses are answered and the eighth is half answered.

---

## §3 — The finding: a FOURTH shape, and it is structural rather than careless

This is the DDR-1084 §1 family — a DDR ships a remedy and does not revisit the
row the remedy answers — and three shapes were already on record:

- **DDR-1084 §1** — the unblocking DDR **never opens the row's file**.
- **DDR-1086 §1** — it opens **one** of the files that records the claim, not the others.
- **DDR-1107 §2** — it opens the **right** file, edits it, and touches only the
  **mechanical** cell (the free-range carrier), never the semantic row ~1,400
  lines above.

**This one is sharper than all three, and it was found by reading DDR-1035's own
header rather than by assuming:**

> **DDR-1035 §0:** *"The design for this was written and committed before the
> code, but as **`docs/PRE_LAUNCH_CHECKLIST.md` §1.5** (commit `e99be3a`) rather
> than as a numbered DDR — it was written as the safety confirmation the operator
> asked for before implementation. This file is the formal record and adds the
> measured evidence. **Saying so rather than back-dating a design doc.**"*

**DDR-1035 cited the row by number, named it as its own design document, and
left it in the future tense.** Not forgotten, not missed, not unknown — *quoted*.

**AND THE REASON IS A GENUINE CONFLICT, WHICH IS WHY THIS IS NOT A CRITICISM OF
DDR-1035.** A design document is written in the future tense **by construction**,
and DDR-1035's instinct to leave it alone is the correct one under
§NON-NEGOTIABLE 5 — back-dating a design doc to read as though it had always
described the finished thing destroys the ordering evidence the rule exists to
preserve. DDR-1081 §5 reached the same conclusion from the other side, where the
obvious tidy-up ("make every stated pair current") **would have destroyed a
correct historical record**.

**The conflict is that ONE PIECE OF TEXT IS DOING TWO JOBS THAT DEMAND OPPOSITE
TREATMENT:**

- as a **design record**, it must stay frozen, in the tense it was written;
- as a **Section 1 status row**, it must be current, because Section 1 is the
  section a person reads to learn what is still owed.

Nothing was done wrong at either step. The document simply cannot satisfy both
without saying which it is.

**Two instances is not a rate and none is claimed.** What is new here is the
shape, not a frequency.

---

## §4 — The genuine residual: the apt half is undone. CORRECTED, NOT CLOSED

Measured, not assumed: **`grep -nE 'actions/cache|cache:|~/.cargo|/var/cache/apt'`
over `ci.yml` returns NOTHING.** There is no caching anywhere in the workflow.

So of §1.5's final paragraph — *"Toolchain caching (apt + rustup) carries neither
hazard … and is the safer half to do first"*:

- **rustup: better than done.** It is not cached, it is **gone** from all ten
  shards. A cache still installs; a removal does not. The build job's own comment
  states the reason — it is the *only* consumer of the Rust toolchain in the
  repository, linking a `no_std` parity ELF, and **nothing in `kernel.bin`
  depends on it**.
- **apt: genuinely not done.** Every shard still runs `apt_prepare.sh` with a
  twelve-package list.

**The row is therefore CORRECTED, NOT CLOSED**, and that distinction is the whole
point of writing this down rather than ticking it: closing it would over-claim a
half that is measurably absent, and leaving it untouched hides a shipped
mechanism that the release evidence already depends on. The `smoke-horizon`
shape (DDR-1071 §3), reached a fifth time.

**No apt cache is built here and none is designed.** It is a `ci.yml` change,
which resets the 3-greens count on a release that is HELD — cheap today,
and still not this session's to spend without a reason better than tidiness.

---

## §5 — A second finding: §1.3's deferral REASON for PRs #7/#8 is partly spent, and the VERDICT stands anyway

§1.3's disposition for PR **#7** (`actions/checkout` 5 → 7) gives **two**
independent reasons to defer:

> "Any `ci.yml` change alters the environment every gate runs in, and
> §NON-NEGOTIABLE 1 needs **3 greens on one tip SHA** — so merging this resets
> accumulated release evidence for no release-relevant gain. **It also touches
> the same file the operator's own item 5 (caching + shared build artifact) will
> rewrite, so merging now buys a conflict.**"

PR **#8** (`actions/setup-python` 5 → 7) is deferred *"same reason as #7"*.

**The second reason is largely spent: item 5's big half HAS now rewritten
`ci.yml`** (DDR-1035 restructured the shard job wholesale — download, touch,
two assertions, no build). What remains of item 5 is the apt cache from §4,
which is additive and conflicts with an `actions/checkout` bump far less than a
pending wholesale rewrite would.

**THE VERDICT DOES NOT MOVE**, because the *first* reason never depended on the
second: a `ci.yml` change still resets the three-greens count for no
release-relevant gain, and that is true today exactly as written.

This is the **DDR-1103 B#1 shape**, third instance: *the verdict is unchanged
and the reason is not, and both halves have to be stated together.* A session
reading only the conflict argument would conclude #7 is now free to merge; one
reading only the evidence argument would conclude it is still blocked. Only the
second is correct, and only saying both makes that checkable.

**No PR is merged, closed, or re-triaged here** — §1.3's own closing paragraph
records why that is the operator's action and not an implementer's, and this
session is mandated to `dev/phase1-seyp3n` in any case.

---

## §6 — A third, small one: Section 1's header never enumerates §1.5

The Section 1 preamble accounts for its rows one by one:

> "**§1.1 and §1.2 are the only rows in this document marked YES** … **§1.3 and
> §1.4** sit here because they are *operator-owned* rather than open …"

**§1.5 is not mentioned.** Checked rather than assumed: `git log -S` shows the
preamble **and** §1.5 were added in **the same commit, `e99be3a`** — so this is
an omission at birth, not drift, and it compounds §1 because a reader who trusts
the preamble's enumeration has no frame for the row at all.

---

## §7 — What changed, and the repair explicitly REFUSED

**Three edits, all at the site** — this DDR's own finding applied to itself, and
DDR-1110 §4's rule (*a correction belongs in the row it corrects*):

1. **§1.5 gains a STATUS header** naming DDR-1035, the date, and the one clause
   that remains open. **The argument text below it is left VERBATIM.**
2. **The preamble** gains one clause placing §1.5, so its enumeration is complete.
3. **§1.3's #7 row** gains a note that the conflict leg is spent and the evidence
   leg is not — keeping the **DEFER** disposition unchanged.

**REFUSED: rewriting §1.5's body into the past tense.** It is the design document
DDR-1035 deliberately declined to back-date, and rewriting it would destroy
exactly the ordering evidence §NON-NEGOTIABLE 5 exists to protect — the DDR-1081
§5 trap, where the obvious tidy-up would have erased a correct historical record.
**A status header above frozen design text costs three lines and loses nothing.**
That is the cheap substitute, and it generalises: **when one document is both a
design record and a status row, date-stamp the status and leave the design
alone.**

---

## §8 — No checker is built, and the reason is measured

The available mechanical signal is *"a checklist row says 'should' or 'would'
about something that exists in the tree"*. That is **semantic**: §1.5's future
tense is **correct as a design document** and **wrong as a status row**, and
nothing in the tree can tell which job a paragraph is doing — the same wall
DDR-1071 §5, DDR-1072 §2, DDR-1081 §3, DDR-1086 §4 and DDR-1107 §3 each measured
and each refused a checker for.

`ci-docstate-check` remains the shape that works, for the reason DDR-1063 gave:
it asserts an **arithmetic identity** between two numbers in a document.
Everything here is prose.

**The cheap substitute, sharpened from DDR-1107 §3:** that rule attached the
obligation to *shipping a named remedy*. Add the narrower trigger this case
supplies — **a DDR that CITES a document section as its own design has, by
construction, taken custody of that section's tense.** DDR-1035 cited §1.5 by
number; that is the moment the status header was owed.

---

## §9 — NOT CLAIMED

- **NO code change, NO gate, NO new sentinel.** `kernel.bin` is **not rebuilt**,
  so the size/headroom pair and `ci-docstate-check` are unaffected;
  `GLOBAL_FORBIDDEN` **77**; **179** gates; **79** probe ELFs.
- **NO defect found in any code and none alleged.** `ci.yml`, `apt_prepare.sh`,
  the hash manifest, the `touch`, both assertions and the per-shard
  `smoke-selftest` are all correct for what they were built to do. **What is
  corrected is the ROW.**
- **DDR-1035 IS NOT CRITICISED.** Its work is real, its measurement is sound, its
  refusal to back-date the design doc is the *correct* call under §NON-NEGOTIABLE
  5, and §3 argues the conflict is structural rather than an oversight on its
  part.
- **§1.5 IS CORRECTED, NOT CLOSED** — the apt-caching half is measurably undone
  and is stated as open.
- **NO apt cache is built and none is designed**; no `ci.yml` change of any kind
  is made here.
- **NO PR is merged, closed, or re-triaged.** §1.3's dispositions stand exactly
  as recorded; only the stated *reason* for #7/#8 is annotated, and the **DEFER**
  verdict is untouched.
- **NO operator decision is taken.** §1.1 (branding) and §1.2 (the `v1.0.0` hold)
  are not revisited and are not this session's to settle.
- **NO rate and no new pattern frequency claimed** — §3 names a fourth *shape*
  from **one** instance, and says so.
- **NO gate was run.** What was measured: `ci.yml` read in full for the `build`
  and `build-and-boot` jobs; greps for `make image|musl|lwip|ci-probe-rodata-check`
  and `rustup` inside the shard job (both hits are comments); a tree-wide grep for
  `actions/cache|cache:|~/.cargo|/var/cache/apt` (**zero**); `fail-fast` and
  `smoke-selftest` occurrences; DDR-1035 §0 and §1 read in full; `git show --stat`
  on `e99be3a`; and two `git log -S` probes establishing that the preamble and
  §1.5 share a birth commit. The build job's published digest for `704b2a5`
  (`68e74ff4142f7c71…`, byte-identical to the local `build/kernel.bin`) was read
  from the live job log, which is what §1 means by "leaned on twice".
- **No open issue moves** (OPEN-1/2/12/13 untouched). Not an apfreeze, not OPEN-2.
