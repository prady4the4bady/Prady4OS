# DDR-1088 — The panic report has never reached a CI job log

**Status:** implemented
**Date:** 2026-09-07
**Branch:** `dev/phase1-seyp3n`
**Class:** instrument defect. **No kernel change. No cause is named for the
panic in §1 and none is attempted (§NON-NEGOTIABLE 3).**

---

## 1. The artefact — two independent shards, one tip, both carrying a panic

Tip `e10f494` (DDR-1087). Both shards printed `kernel.bin: OK` (DDR-1035), so
both ran the intended binary; rebuilt here bit-for-bit at `81b379053094041c`,
1,307,018 B, and every address below is resolved against **that** build
(§INV.18).

**Shard 7 — CI 34089554836, job 101641066783, `smoke-blkmq`, failed after 3 of 18:**

```
3 forbidden patterns matched this ONE capture (DDR-1079)
  matched: multi-inflight FAIL
  matched: NEXUS KERNEL PANIC
  matched: panic_stage=
[hb] t=500 ... panics_silent=0 panic_stage=3 loser_cpu=0 loser_vec=0 loser_rip=0x0000000000000000
[blk] multi-inflight FAIL done=0x0000000000000000 spawned=2/2
```

**Shard 3 — CI 34089556866, job 101641095640, `smoke-rqstress-liveness`, failed after 1 of 21:**

```
  matched: [apfreeze]
  matched: NEXUS KERNEL PANIC
  matched: panic_stage=
[apfreeze] cpu=2 ticks=163 rip=0xFFFFFFFF8000C7B7 if=0 pid=22 shot=1..4
  shot=2 bt=0xFFFFFFFF80014D41,0xFFFFFFFF8000C84F,0xFFFFFFFF8000BAAA,0xFFFFFFFF8000027A
[hb] t=500/1000/1500 ... panics_silent=0 panic_stage=3 loser_cpu=0 loser_vec=0 loser_rip=0x0
[vblk] compl wait timeout unit=1 dest_cpu=2 dest_dticks=0 dest_abs=163 bsp_abs=1185
       ticks[1185,1164,163,1161] on_cpu=0 lba=32
[yieldstall] site=mnt_lock spins=141791/53967/25519
```

Resolved against the exact binary:

| address | symbol |
|---|---|
| `0xFFFFFFFF8000C7B7` | `isr_dispatch+0xfe7` |
| `0xFFFFFFFF80014D41` | `sched_tick+0x2b1` |
| `0xFFFFFFFF8000C84F` | `timer_tick+0x7f` |
| `0xFFFFFFFF8000BAAA` | `isr_dispatch+0x2da` |
| `0xFFFFFFFF8000027A` | `isr_common.gs_kernel_in+0x8` |

### 1.1 What that does and does not establish

**It is NOT DDR-1079's shape.** There the winner and the loser were the same
CPU: `panics_silent=1`, and `loser_rip` resolved into the panic backtrace
walker. Here `panics_silent=0` — **no CPU lost the CAS**, so exactly one CPU
entered the panic path and nothing re-entered it — and the frozen CPU's RIP is
inside `isr_dispatch` under a **timer** backtrace, not DDR-1019's
`for(;;) cli; hlt`. So the frozen CPU never reached the panic path at all.
That backtrace is **DDR-1006's producer** (an AP inside its own timer ISR),
which is OPEN-2's reopened form, resolved rather than colour-matched.

**Measured, not inferred:** CPU 2's ticks stop at **163** while the BSP reaches
**1185**, and `dest_cpu=2` matches `loser`-independent `[apfreeze] cpu=2`, so
the stranded unit-1 completions are downstream of that freeze.

**Both events precede t=500** — `panic_stage` already reads 3 at the first
heartbeat, and the freeze is at tick 163. **Which came first is NOT
established**, and §2 is why: the banner's position in the capture would say,
and no job log carries it.

**One further fact, recorded because it is unexplained rather than because it
supports anything:** shard 7 declared a `FORBIDDEN_SENTINEL`, so per DDR-1043 it
is **not** early-exit eligible and burns its full window by design — yet its
QEMU ended **10.3 s into a 180 s window** (06:19:43.16 → 06:19:53.48) having
reached `t=1000`. The harness did not stop it; the guest exited. That is
consistent with a triple fault under `-no-reboot` (`boot_test.sh:626`) and is
**not established**, because the report that would say is the thing §2 is about.
Shard 3, by contrast, was killed by `timeout` at its full window.

---

## 2. THE FINDING — the scan names the panic and prints only its banner

DDR-1079 fixed the scan returning at the **first** matching pattern, so a run in
which a CPU panicked was reported as a block-integrity failure and the panic was
never named. Both captures above show that fix working: all three patterns are
named. **And in both, everything the panic actually said is still absent.**

`check_global_forbidden` (`boot_test.sh:485`) has two printers and **both run in
the same direction**:

1. per matched pattern — `grep -aF "$p2" "$SERIAL_LOG" | head -3`: the matching
   **lines only**, no context;
2. first matched pattern only — `grep -aB40 -m1 -F "$pat"`: **leading** context.

DDR-824 chose leading context for a stated and correct reason: *probes are
written summary-last* — `[sfs] churn FAIL op=create iter=17` precedes the
summary line the list matches on, so the diagnosis is **above** the match.

**The panic is written summary-FIRST.** Measured on a real capture
(`build/mce.log`, from `smoke-mce`, the one gate whose pass condition is a
panic): banner at line 111, `halting.` at line 143 — **33 lines, every one of
them after the banner**: `component:`, `exception: <name> vector= error=`, the
`MCE:` block for vector 18, `RIP=`/`CS`/`RFLAGS`/`RSP`(+`CR2` for #PF), sixteen
GP registers, `backtrace:`, the frames, `<frame chain ends: fp=…>`.

So for the one pattern whose match line carries **no information at all** —
`*** NEXUS KERNEL PANIC ***` is exactly the string `GLOBAL_FORBIDDEN` already
told us matched — the printer shows that line and stops. And the leading-context
block cannot rescue it, because it goes to the **first** pattern in list order,
and the two patterns that name a panic sit at positions **71 and 72 of 76**
(measured), while `[apfreeze]` is **2** and `multi-inflight FAIL` is **58**.

**Consequence, and it is the reason this is worth a commit:** DDR-1079 built a
bounded panic backtrace walker so that a garbage frame pointer could no longer
kill the CPU mid-report — and **no CI job log has ever printed a frame from it**,
because its output is trailing context of a pattern that only prints its own
matching line. DDR-1079 §2 had to resolve its RIP by hand out of `loser_rip=` in
a heartbeat. The same is true here, and here there is no `loser_rip` to use.

### 2.1 `scan_forbidden.sh` has the same defect, worse

`tools/qemu_runner/scan_forbidden.sh` (DDR-1010 §8) — the scanner for gates that
do **not** go through `boot_test.sh`, i.e. `smoke-shell` (a mandatory hygiene
gate) and `smoke-mce` arm F — prints `grep -aF -- "$p" "$log" | head -3` and
**nothing else, in either direction**. It has no `-B40`. Both scanners are fixed
here, in one commit, because two copies of one printer drift (DDR-1037's
`fd_ready_mask` reasoning).

---

## 3. Why this is NOT a re-ranking problem

The tempting fix is to rank the patterns so a cause outranks a symptom and the
detail block goes to the panic. **DDR-1079 refused that deliberately** and the
refusal still stands: it would be "one more list to keep in step with 76
patterns and would drift". A summary-first / summary-last classification per
pattern would drift in exactly the same way.

The fix here needs **no per-pattern knowledge**: context is printed in **both
directions**, for **every** match, so the printer never has to know which kind
of thing it matched. Uniform, and there is no second list.

---

## 4. The obvious arm is vacuous, and that was measured before it was written

Eighth time this class has been caught in design text (DDR-1039 §3.1, 1058 §2,
1067 §2, 1070 §4, 1083 §4, 1084 §4, 1085 §2.1).

The natural fixture is "put a panic in a capture, scan it, assert `RIP=` appears
in the output". **That passes today, with no fix at all**, whenever the panic
happens to sit within the 40 lines of leading context already printed for the
first match. It is only when the panic is *far* from the other matches that the
current printer loses it — which is precisely the geometry of §1 (panic before
t=500, `[apfreeze]` after t=1500, hundreds of lines apart).

So the fixture places a full 33-line panic report near the top of a capture,
**60 filler lines**, then an `[apfreeze]` line — which, at list position 2,
becomes the *first* match and takes the leading-context block, reproducing §1
exactly. The assertion is on a line **32 lines after the banner**, which no
leading window can reach.

---

## 5. The fix

Additive in both scanners: nothing that prints today stops printing. A change to
the failure reporter must not remove evidence a past investigation relied on
(the same argument DDR-1087 used for the line source).

* `boot_test.sh` `check_global_forbidden`: after each matched pattern's existing
  `head -3` matching-line summary, print a bounded **trailing** window from that
  pattern's first occurrence.
* `scan_forbidden.sh`: the same, after its `head -3`.

**Window = 40 lines, sized by measurement not by taste:** the real report is 33
lines with 6 frames, and the walker is bounded at **8** frames
(`idt.c:969`, `for (int i = 0; i < 8; i++)`), so the worst case is ~35. Forty
gives margin without dumping a boot. Cost is bounded and lands only on runs that
are already red: matches × 40 lines, and matches have been 1–3 in every capture
on record.

---

## 6. Two mutants, landing on different arms, neither carrying the other

The DDR-1044 M2/M3 check.

* **M1 — the pre-fix tree itself** (trailing window removed). Not a synthetic
  defect: it is literally the reporter as it stood before this commit, the
  DDR-1066/1067 form. Fails the new case.
* **M2 — the window shrunk to 5 lines.** Measured, not predicted: the run
  reports *only* `missing: halting.` — the **near** arm (`component: NEXUS isr`,
  one line after the banner) still passes, and the **far** arm (`halting.`,
  thirty-two lines after it) **fails alone**. Its output is the argument for the
  two-arm form: it prints `component:`, `exception: #PF page fault vector=0x0E
  error=0x02`, `RIP=` and `CS =` — four entirely plausible lines of a panic
  report — so a check asserting only that *something* followed the banner would
  have shipped it. Neither mutant carries the other: M1 fails both arms of both
  cases, M2 fails one arm of each.

**Measured offsets, corrected from this DDR's own first draft:** an earlier
version said `RIP=` sits six lines after the banner. That is true only for
vector 18, where the two `MCE:` lines precede it; for the `#PF` in the fixture
it is at +3. The near arm was therefore moved to `component:` at +1, which is
the one offset that does not depend on the exception type.

---

## 7. Proof

`smoke-selftest` — the DDR-791 meta-test that exists to catch exactly this
machinery being silently broken — goes 7 cases to 9, all PASS. Regression on the
gates that drive each scanner: `smoke-shell` (a mandatory hygiene gate, and a
`scan_forbidden.sh` consumer), `smoke-mce` (arm F, the other consumer, and the
only gate whose pass condition is a panic), `smoke-selftest`, `smoke-fs`,
`smoke-blkmq` — **all rc=0**, with `kernel.bin` `81b379053094041c`,
1,307,018 B, **identical before and after**, which is the appropriate proof for
a change that touches no kernel source. Hygiene **ALL EIGHT**.
`GLOBAL_FORBIDDEN` verified **76**.

`smoke-blkmq` is worth naming: it is the shard-7 red from §1, and it passes
locally on this exact binary. That bounds nothing (one run), and it is reported
so its absence is not mistaken for an untried check — not as evidence about the
CI failure.

---

## 8. The four-carrier warning was itself on one carrier

Found while advancing the DDR free range for this commit, and it is DDR-1086's
own defect one level down.

DDR-1086 §3 established that the free range lives in **four** places — three in
`CLAUDE.md` (§INV.4, §CURRENT BUILD STATE, §ORIENTATION) and one outside it
(`docs/PRE_LAUNCH_CHECKLIST.md` §6) — and that "update all three" is why the
fourth kept going stale. It then added a warning naming the fourth carrier **to
the §CURRENT BUILD STATE copy only**. Measured now: §ORIENTATION still read
*"§INV.4 and §CURRENT BUILD STATE carry it too; all three must be updated
together"*, and §INV.4 carried no count at all.

One commit later, DDR-1087 advanced **exactly three** and left the checklist at
`DDR-1087+` while `CLAUDE.md` read `DDR-1088+` — the same defect, five days'
worth of DDRs after it was named and fixed.

**A warning about a carrier that gets missed is itself missed when it lives at
only one of the carriers.** The count is now stated at every carrier, which is
the cheap substitute; no checker is built, and DDR-1086 §4's refusal stands
unchanged (nine of the ten stated ranges naming an occupied number are correct
historical records, so a mechanical check reddens on nine to catch one).

Severity is unchanged and is stated rather than dramatised, as DDR-1086 §3.1
did: §NON-NEGOTIABLE 8 requires an `ls` of both DDR directories before
allocating, so a stale range costs a lookup and not a collision.

---

## 9. NOT CLAIMED

* **No cause is named for the panic in §1 and none is guessed.** §NON-NEGOTIABLE
  3 forbids a fix without a named mechanism, and the mechanism is precisely what
  the unreadable report withholds. What changes is that the **next** occurrence
  can say.
* **OPEN-2 is not closed and no rate is claimed.** Two occurrences is not a rate,
  and no campaign is run to manufacture one.
* **NOT attributed to `e10f494` and NOT exonerated.** "The diff is elsewhere" is
  not an argument (DDR-1042). And this is *not* a same-binary observation in
  DDR-1009's strict sense: `e10f494` is ring-3 only but `prism.elf` is embedded,
  so `kernel.bin`'s hash moved (`d4b148faaca8ce09` → `81b379053094041c`) while
  its size did not. Stated rather than leaned on.
* **DDR-1079 is not accused of anything.** Its fix is what made both captures
  name the panic at all, and its bounded walker is what this change finally
  makes visible. Its refusal to hand-rank patterns is upheld, not reversed (§3).
* **No kernel change**: `kernel.bin` is untouched at 1,307,018 B /
  `81b379053094041c`, so the CLAUDE.md size/headroom pair and `ci-docstate-check`
  are unaffected.
* **No new gate** (178 unchanged); `GLOBAL_FORBIDDEN` **76**, unchanged — no
  pattern is added, the change is to how matches are *reported*, exactly as
  DDR-1079's was.
* **No open issue moves** (OPEN-1/2/12/13 untouched).
* The shard-7 early guest exit (§1.1) is **recorded, not explained**.
