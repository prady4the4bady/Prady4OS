# DDR-1135 — THE HUNT'S PRINTER SHOWS MATCHING LINES ONLY, SO A PANIC IS REPORTED BY THE ONE LINE THAT SAYS NOTHING

**DESIGN COMMITTED BEFORE THE CODE (NON-NEGOTIABLE 5).** This file is the design and the
measurements it rests on; the implementation, the fixture and the mutants follow in the
next commit and their results are appended to §7 here.

**No kernel change. `kernel.bin` is not rebuilt. NO FIX FOR OPEN-2, no mechanism named,
OPEN-2 does not close, no open issue moves.** What is corrected is a **reporting** path.

---

## §1 — THE ARTEFACT

DDR-1134 §5: hunt run 35588431931 lane 12 produced a real `NEXUS KERNEL PANIC` with
`panic_stage=3 panics_silent=0` — one CPU panicked, nothing re-entered — whose
`[apfreeze]` RIP resolves to `isr_dispatch+0xfe7`, disassembling in that binary to
`callq kputs / cli / hlt / jmp`: DDR-1099's fifth producer, the **winner's terminal halt
at the end of a COMPLETED panic report**.

So the report was **written**. It is not in the job log. Measured: the banner sits at
capture line **215**, the next line the job log shows is **241**, and `component:`,
`exception:`, `vector=`, `RIP=`, `CR2`, `backtrace` and `halting.` **all count zero.**
The ~25-line register block — the only thing that could name the exception — is gone.

**DDR-1134 §5 therefore had to record the cause of that panic as NOT NAMED AND NOT
GUESSED.** That is the cost, and it is the second time this project has paid it.

## §2 — THE MECHANISM, AND IT IS DDR-1088's DEFECT IN A THIRD PRINTER

`tools/ci/open2_hunt_campaign.sh:141`:

```sh
grep -nE "$SIGNALS" "$cap" | head -40
```

**Matching lines only, no context in either direction.**

DDR-1088 found exactly this and stated the reason it is fatal specifically for a panic:
**probes are written summary-LAST and a panic is written summary-FIRST.** So for the one
pattern whose matching line carries no information at all — the banner — the printer shows
that line and stops.

DDR-1088 fixed `check_global_forbidden` **and** `scan_forbidden.sh` **in one commit**,
and said why in as many words: *two copies of one printer drift* (the DDR-1037
`fd_ready_mask` reasoning). **The hunt has a third copy**, and DDR-1088 did not touch it.
DDR-1129 later raised its cap from `head -5` to `head -40` — a correct fix for the problem
DDR-1129 had, four `[apfreeze]` shots truncated — **without giving it context**, because
truncation and context are different defects and only the first was in front of it.

**This is the DDR-1117 / DDR-1130 shape once more: a defence that exists, is justified, is
documented, and is not present on the path actually in use.**

## §3 — TWO MEASUREMENTS AT THE SITE, NOT AN ARGUMENT

**(a) The cap was at its boundary with zero margin.** `grep -c` reported **40**, the cap is
**40**, and exactly **40** lines printed — so nothing was truncated *this time*. One more
matching line and the cap eats it silently, with `sig=41` against 40 printed as the only
tell. DDR-1129 raised the cap precisely because 5 was one short of the observed 5; the same
edge has returned at 40.

**(b) 35 of the 40 slots went to `[hb]` heartbeat lines.** They match because
`panic_stage=` appears on **every heartbeat** once a panic has been claimed. That is
DDR-1049's design working exactly as intended and **no defect is alleged in it** — that
field is in `SIGNALS` because it is the one detector that fires when a lone CPU panics and
dies *before* its banner, leaving every other channel empty.

**But it makes `panic_stage=` the best available DETECTOR and the worst available PRINT
TARGET at the same time.** The printer spends ~87% of its budget on lines that are not
events and discards the one block that is. **Separating those two roles is the whole
design.**

## §4 — THE DESIGN

**Detection does not move.** `SIGNALS` is unchanged, `sig` is still `grep -cE "$SIGNALS"`,
the signal/clean verdict and `signal_runs` are untouched. **Nothing about what counts as a
signal changes**, so no past or future run's counts are affected.

**The existing index is kept verbatim** — `grep -nE "$SIGNALS" "$cap" | head -40` — so
nothing that works today stops working, and **this is the property that makes §5's filter
safe**.

**Added: a bounded, merged context window around each non-heartbeat match.** Leading **and**
trailing (DDR-1088's fix), because the whole finding is that the report follows its banner.

## §5 — WHY EXCLUDING `[hb]` FROM THE CONTEXT PRINT IS NOT THE KNOWLEDGE DDR-1079 REFUSED

DDR-1079 refused to re-rank `GLOBAL_FORBIDDEN` into cause/symptom order, calling it *"one
more list to keep in step with 76 patterns"* — a list that drifts. **That refusal stands
and is not revisited.** This is not that:

1. **`[hb]` is one stable token, and this script already depends on it twice** — line 126's
   vacuity check (`grep -q '\[hb\]'`) and line 131's `grep -o '^\[hb\] t=[0-9]*'`. It is
   not new knowledge; it is knowledge the file already has.
2. **The filter cannot hide anything, and that is the load-bearing safety property:** the
   *unfiltered* index still prints every match, heartbeats included. The filter decides only
   which matches are worth surrounding with context, never which are reported.
3. It is a statement about **one line's shape**, not about which patterns are causes and
   which are symptoms.

## §6 — VACUITY CHECKED BEFORE THE ARM WAS WRITTEN

**(a) "Assert the panic body appears" is NOT vacuous here, and the reason is worth stating
because it nearly was.** The index prints *only matching lines*, never context, so on the
unfixed script no non-matching line can reach the log at all. **But that holds only if the
panic body contains no `SIGNALS` token** — otherwise the index would print it and the arm
would pass with no context whatsoever. A real report does not (`component:`, `exception:`,
`RIP=`, the registers, `backtrace`, `halting.` match nothing in `SIGNALS`), and **the
fixture must assert that property of itself** rather than assume it.

**(b) The fixture must reproduce lane 12's GEOMETRY, not merely contain a panic.** A
fixture whose panic is the only match passes on a printer that prints one window around one
match and would still fail on a real capture flooded with 36 heartbeats. So: ~214 filler
lines, the banner, a body carrying no `SIGNALS` token, then the heartbeat flood and the
`[apfreeze]` shots.

**(c) The mutant that carries the claim is LEADING-CONTEXT-ONLY.** That is DDR-1088's
original defect exactly, it is the plausible wrong implementation, and it **passes any arm
that merely asks whether context was printed**. The arm must assert a line that falls
*after* the banner.

**Planned mutants** (results in §7): **M1** = the pre-fix script verbatim, must fail the
body arm — the DDR-1066/1067 form, a real prior state rather than a synthetic defect;
**M3** = leading context only, must fail the body arm while still printing context.
The output bound is asserted directly in the fixture rather than as a separate mutant.

## §7 — RESULTS

**Implemented.** The printer is moved out of the campaign into `tools/ci/hunt_print.sh`,
which the campaign sources, so **there is one copy of it**. DDR-1088 fixed two copies of the
same printer in one commit because copies drift, and the hunt turned out to be a third copy.
Detection is byte-for-byte unchanged: `SIGNALS`, `sig=$(grep -cE ...)`, the verdict,
`signal_runs` and `churn_runs` are all the same.

**Shipped behaviour:** (1) the index `grep -nE "$SIGNALS" | head -40` is kept verbatim, and
it now prints `index TRUNCATED: N matches, first 40 shown` when the cap cuts it off. Lane 12
filled the cap exactly, and nothing in the output would have said so if one more line had
been dropped. (2) Around every match that is not an `[hb]` heartbeat, it prints a context
window of 5 lines before and 40 after, with overlapping windows merged. The 40 is sized from
DDR-1088's measurement: a real report is 33 lines, and the walker is bounded at 8 frames.
(3) Total context is capped at 240 lines, and hitting that cap prints `context TRUNCATED`.
The code uses POSIX awk only, because this host's awk is mawk (DDR-1079/1121).

**The fixture reproduces lane 12's layout (§6(b)):** 214 boot lines, the banner, a 33-line
body, 36 heartbeats carrying `panic_stage=`, and 4 `[apfreeze]` shots, which gives **41
matches against a cap of 40**. The selftest **checks the fixture's own property (§6(a))**:
the body matches `SIGNALS` exactly once, on the banner. Without that check, arms A and B
could pass with no context printed at all. The selftest also **fails if its copy of
`SIGNALS` drifts from the campaign's.**

**Arms:** A is the near arm (`component: NEXUS isr`, the line after the banner). B is the
far arm (`halting.`, 32 lines after the banner, and the one the claim rests on). C checks
that the index reports its truncation. D checks that the output stays within bounds. E uses
a second fixture, 30 matches 60 lines apart, which is over the cap at any window of 5 or
more, and checks that the context cap reports itself.

| printer | failing arms | expected |
|---|---|---|
| shipped | none | none |
| **M1** pre-fix `grep \| head -40` verbatim | A B C E | A B C E |
| **M2** trailing context 5 | **B only** | B |
| **M3** leading context only (DDR-1088's original defect) | A B | A B |

**Each mutant fails a different set of arms.** M3 is additionally checked to have printed
leading context (`filler line 213`), because a leading-only printer that printed nothing
would not be the defect it is meant to model. **M2 corrected the fixture:** with 20 spread
matches, M2's narrower windows fitted under the context cap, so it also failed E. That
measured a property of the fixture, not independence between arms, so the spread fixture
was widened to 30 matches to go over the cap at any window of 5 or more.
Arm D is asserted directly on every run, as planned, and has no mutant of its own.

**Measured on a REAL panic capture, not only the fixture.** `make smoke-mce` produced a
genuine `#MC` panic report (`build/mce.log`, 115 lines, kernel `25f4dae4a3f90bcb`, rc=0).
Both printers were run over it:

| | lines printed | `component:` | `RIP=` | `backtrace` | `halting.` |
|---|---|---|---|---|---|
| before (pre-fix printer) | **1** | 0 | 0 | 0 | 0 |
| after | 37 | 1 | 1 | 1 | 1 |

**Before, the printer showed one line: the banner.** That is DDR-1134 §6's finding
reproduced on real kernel output rather than argued from it.

Wired as `make ci-huntprint-selftest` into `hygiene_check.sh` (**ALL NINE**) and into
`ci.yml`'s `shard-check` job, next to the other toolchain-free checks.
`build/kernel.bin` is untouched (`25f4dae4a3f90bcb`) and was re-hashed after every QEMU run.

## §8 — NOT CLAIMED

- **NO FIX FOR OPEN-2**, no mechanism named, OPEN-2 does not close, no open issue moves.
  This changes what a **future** capture can say and nothing about any past one — lane 12's
  panic stays **unexplained**, and its report is in a per-lane artifact this container
  cannot fetch (the proxy refuses `productionresultssa*.blob.core.windows.net`;
  `/root/.ccr/README.md` says to report the blocked host rather than route around it, and
  that is what is done).
- **NO defect alleged in `SIGNALS`, in `panic_stage=`'s membership of it, or in DDR-1049's
  heartbeat design** — §3(b) is that instrument working as built.
- **DDR-1088 is NOT criticised**; its fix is correct and is what makes this legible as a
  *third* copy rather than a regression. **DDR-1129 is NOT criticised**; its cap raise was
  the right fix for the defect in front of it.
- **DDR-1079's refusal is NOT revisited** — §5 argues this is a different thing, not that
  that refusal was wrong.
- **Detection is unchanged**, so no past run's `signal_runs` or `churn_runs` moves and no
  rate is revised.
- **A note owed about the in-flight run, stated rather than left to be noticed.** Run
  35643638290 was dispatched 19:15Z; at the time this was written **19 of its 20 lanes had
  completed `actions/checkout` and lane 9 was still queued**. The workflow checks out
  `ref: dev/phase1-seyp3n` **by branch name**, so lane 9 will take whatever the tip is when
  a runner frees — i.e. **lane 9 may run this printer while the other nineteen run the old
  one**. That is **not** DDR-1060 §9's hazard: the campaign script is **not a build input**
  (the kernel builds from `a390eab`, unchanged, and the workflow's own step 8 asserts the
  pin), `SIGNALS` is unchanged so detection and `signal_runs`/`churn_runs` are identical
  either way, and the captures are uploaded regardless. **The only difference is job-log
  rendering**, and it is recorded here so a reader who notices two formats in one run knows
  why rather than inferring a defect.
