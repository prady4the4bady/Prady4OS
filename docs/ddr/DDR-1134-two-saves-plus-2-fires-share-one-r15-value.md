# DDR-1134 — BOTH `disp = saves + 2` FIRES CARRY ONE SPECIFIC `r15`, AND THE HUNT'S OWN PRINTER DISCARDED A PANIC REPORT

**Measurement + address resolution + a design. NO CODE CHANGE, no gate, `kernel.bin` NOT
rebuilt in the shipping tree. NO FIX, NO MECHANISM NAMED, OPEN-2 DOES NOT CLOSE, no open
issue moves (OPEN-1/2/12/13 untouched).**

---

## §1 — THE ARTEFACT, AND THE DISCRIMINATOR RUN FIRST

Run **35588431931**, `workflow_dispatch`, 20 lanes × 50 runs, `tree_sha=d280b809…`,
`kernel_pinned=ca8107ec7f5d8de7`. Finished 2026-09-21T15:53Z, `conclusion=failure` —
which on this workflow means **it found something** (the file's own header: *"IT FAILS
WHEN IT FINDS SOMETHING … Do NOT fix it to pass on a hit"*), so reading the conclusion
as a verdict would assume the thing being measured.

**DDR-1132's discriminator was applied BEFORE a single log was opened: 4 of 20 lanes
non-success** — lanes 0, 7, 12, 15. Not all 20, so this is **not** a setup failure. It
is the largest signal count any dispatch has produced (DDR-1128: 1 of 6; DDR-1133: 2 of
21, twice).

**DDR-1130's no-filesystem trap is not in play**, quoted from each lane's own line rather
than carried:

| lane | `[campaign] DONE` | capture |
|---|---|---|
| 0 | `runs=50 signal_runs=1 churn_runs=49 kernel_pinned=ca8107ec7f5d8de7` | `run-24.log.fail-8560` |
| 7 | `runs=50 signal_runs=1 churn_runs=50 kernel_pinned=ca8107ec7f5d8de7` | `run-3.log.fail-5425` |
| 12 | `runs=50 signal_runs=1 churn_runs=49 kernel_pinned=ca8107ec7f5d8de7` | `run-18.log.fail-7803` |
| 15 | `runs=50 signal_runs=1 churn_runs=50 kernel_pinned=ca8107ec7f5d8de7` | `run-46.log.fail-12106` |

**A denominator stated honestly rather than assumed.** Only failed-job logs are fetched,
so **200 boots are verified by the four `DONE` lines above**; the other sixteen lanes'
`DONE` lines are **unread**, and "1000 boots" rests on the dispatch parameters, not on
twenty read lines. Recorded because DDR-1133's own closing commitment was to quote each
lane's line rather than carry a figure.

## §2 — §INV.18 SATISFIED BY REBUILD, NOT BY ASSUMPTION

`a390eab` (DDR-1126, the last commit touching a build input — established by
`git log -1 -- Makefile 'kernel/**' 'boot/**' 'user/**' 'arch/**'`, not guessed) rebuilt
at `OPEN2_HUNT=32` reproduces **`ca8107ec7f5d8de7`, 1,319,306 B, BIT-FOR-BIT**,
warning-clean at `-Werror` (0 matches for `\b(error|warning):`).

Built in a **detached worktree** so the shipping tree's `build/` was never touched
(DDR-1119's discipline; DDR-1060 §9's void hazard avoided) — verified: the shipping
`kernel.bin` read `25f4dae4a3f90bcb` before and after. Worktree removed afterwards: a
**measurement aid, not a release artefact**.

**One reconstruction step is recorded because it is load-bearing.** A `git worktree` does
not carry submodules, so the first build died at
`No rule to make target 'third_party/musl/Makefile'`. The two submodules were linked from
the main tree — and **that is legitimate only because the pointers were checked, not
assumed**: `git ls-tree` gives identical hashes at `a390eab` and `HEAD` for both
`third_party/musl` (`0784374d…`) and `third_party/lwip` (`77dcd25a…`), and
`git submodule status` shows the main tree checked out at exactly those commits. **The
bit-for-bit hash match is what proves the reconstruction correct**; the pointer check is
what made it worth attempting. Every address below resolved in **python3, never awk**
(DDR-1079/1121).

## §3 — THREE `[schedcheck]` FIRES, ONE FROZEN CPU EACH

Lanes 0, 15 and 7 all halt at `rip=0xFFFFFFFF80016D91` = **`schedule_locked+0x671`**, the
deliberate `hlt` at `sched.c:1857` reachable only from inside DDR-1105's check. Resolved
**by identity** — the same pinned binary DDR-1129 resolved — so §INV.18's offset-matching
trap does not arise.

**DDR-1123's per-CPU census reads exactly one frozen CPU in all four lanes**, which is a
real use of that instrument rather than a latch read as a census (DDR-1122's correction):
lane 0 `ticks[0=1500,1=1480,2=1478,3=155]`; lane 15 `[…,2=163,3=1476]`; lane 7
`[…,2=119,…]`; lane 12 `[…,3=158]`.

### §3.1 — Four distinct arrival paths at one halt site

Backtraces, resolved in this binary:

| lane | `disp−saves` | arrival path |
|---|---|---|
| 0 | 1 | `schedule+0x11 ← sched_ap_enter+0x178 ← smp_ap_entry+0x30a ← 0x07FA9023` |
| 15 | 1 | `schedule+0x11 ← sched_exit+0x1af ← sys_exit+0xd9 ← syscall_dispatch+0x131` |
| 7 | **2** | `schedule+0x11 ← yield+0xbd ← reaper_thread+0x108 ← thread_trampoline+0x34` |

Lane 0's `bt[3]` sits **below the first text symbol** (`0xffffffff80000000`) — stack data,
**left unresolved deliberately**, exactly as DDR-1128 §6 predicted.

Lane 0 is DDR-1128/1129's **AP bring-up** path; lane 15 is a **ring-3 `sys_exit`**; lane 7
is the **reaper thread**. With DDR-1133's ring-3 `SYS_YIELD` that is **four unrelated
arrival paths at one halt site**, which **strengthens DDR-1129**: the halt-site
attribution is a property of the SITE, not of any one path.

## §4 — THE FINDING: BOTH `saves+2` FIRES CARRY `r15 = 0xFFFFFFFF80016842`

```
lane 0   tid=11 pid=0   rsp=0x07C32528 base=0x07C30000 rflags=0x07C32558
         r15=0xFFFFFFFF8001668D ret=0x0000000000000002 rq_on=0 disp=30494 saves=30493
lane 15  tid=11 pid=0   rsp=0x07C32558 base=0x07C30000 rflags=0x07C32588
         r15=0xFFFFFFFF8001668D ret=0xFFFFFFFF80016DCF rq_on=0 disp=29777 saves=29776
lane 7   tid=22 pid=22  rsp=0x07D036E0 base=0x07D00000 rflags=0x07D03710
         r15=0xFFFFFFFF80016842 ret=0xFFFFFFFF80016DCF rq_on=0 disp=4     saves=2
```

**`disp = saves + 2` has now been observed TWICE, and both times `rq_on = 0`.** DDR-1118's
mechanism carries a *stated precondition* of `rq_on=1` — DDR-1118 built that field
precisely because a double resume through the runqueue needs the thread queued while
running — and it is **absent 2 of 2**. One absence is a coincidence; two is a narrowing.

**And the two values of `r15` sort exactly with the two values of `disp − saves`:**

- `0xFFFFFFFF8001668D` = **`finish_task_switch+0xd`** — the value the consumed-frame chain
  *predicts* (DDR-1118/1133) — on **both** `saves+1` fires.
- `0xFFFFFFFF80016842` = **`schedule_locked+0x122`** — on **both** `saves+2` fires
  (this one and DDR-1133's fourth fire).

**Disassembled in this binary rather than carried**, which confirms DDR-1133's
identification on an independent rebuild:

```
ffffffff80016831: callq  <rq_push>
ffffffff80016836: movq   -0x28(%rbp), %rdi
ffffffff8001683a: movl   -0x1c(%rbp), %esi
ffffffff8001683d: callq  <switch_wait_offcpu_sched>
ffffffff80016842: cmpl   $0x0, %eax          <-- the r15 value: the RETURN of that call
```

So `schedule_locked+0x122` and "`call switch_wait_offcpu_sched` + 5" are **the same
address expressed two ways** — checked before any correction was drafted, and **none is
owed**.

DDR-1133 §1 established what a value in that slot means: a genuine `context_switch` save
leaves `finish_task_switch+0xd` there, so `0x16842` means **a later, deeper activation on
this same stack overwrote the `this_cpu` litter**. The frames whose dispatch count runs
two ahead of their save count are exactly the frames where a deeper activation went
through `switch_wait_offcpu_sched`.

### §4.1 — THIS IS THE MEASUREMENT DDR-1133 §5 SAID WAS OWED

The chain of custody on this candidate is unusually clean and is why the result counts:
**DDR-1119** named `switch_wait_offcpu_sched`'s bounded handshake as where a third fire
would point *or away from*; **DDR-1131 §2** closed it **by reading** (the bail path
`rq_push`es the contended thread back and substitutes `g_idle[cpu]`, so `dispatches++`
past it belongs to IDLE); **DDR-1133 §5** re-read that argument in full, found it
**CORRECT AS FAR AS IT GOES**, and recorded that a candidate closed on a reading now had
an artefact touching it, so the honest disposition was **RE-OPEN FOR MEASUREMENT, NOT
RE-CLOSE ON THE SAME READING**. This is that measurement.

### §4.2 — STATED AT ITS REAL STRENGTH

**n = 2 against n ≥ 3. A CO-OCCURRENCE, NOT A MECHANISM** (DDR-1056: a matching shape is
not a mechanism). The `saves+1` population is *not* uniform either — DDR-1133 §9's lane 13
read `r15=0x10`, neither value — so the clean statement is: **every observation of
`disp = saves + 2` carries `switch_wait_offcpu_sched`'s return address, and no observation
of `disp = saves + 1` does.**

**What it bears on, and it is DDR-1133 §10.3's reading (ii):** a lost non-atomic
`switches_away++` is a race **in the instrument**, and it has no reason whatever to
correlate with which return address happens to sit in a spent frame's `r15` slot. A
2-for-2 correlation points **away from** "the counters are reading noise" and back at
something structural on that call path. **It does not refute (ii)**, and (ii) is not
withdrawn.

**Two smaller observations, recorded without inference.** Lane 7's **`disp=4`** — a thread
dispatched *four* times, against 38 and 30,494 elsewhere — sharpens DDR-1133 §9.4: whatever
selects a bad `next->rsp` is not confined to long-lived threads, and is now seen on one
that has barely run. And **lane 7 is the reaper thread's own `yield()`** — the thread whose
job is freeing TCBs — which is the shape DDR-996's freed-while-queued family and DDR-1096
§3's recycled-TCB hypothesis describe; **n=1 on that path, NOT attributed, and the two
`saves+2` fires have DIFFERENT arrival paths** (ring-3 `SYS_YIELD` for DDR-1133's,
`reaper_thread` here) while sharing the frame signature, so the constant thing is the
frame, not the path.

### §4.3 — A FOURTH `ret` CLASS

DDR-1116 built `ret` because **its legal set has exactly two members**, so it discriminates
a real frame overwritten from a pointer addressing no frame. Six `[schedcheck]` fires on
this one binary have now produced **four** values: `0x16DCF` = `schedule_locked+0x6af`
(post-`call local_irq_restore`, consumed-frame litter, ×3), `0x16DC1` =
`schedule_locked+0x6a1` (post-`call context_switch`, **a legal value** — what a fresh save
carries), `0x0` (neither), and now **`0x2`** (lane 0). DDR-1133 §9.2 found three classes
and concluded that pooling these fires as one signature had become *a choice rather than a
default*; four sharpens that. **No mechanism is named for any of the four**, and it is
**not** claimed they are the same defect nor that they are different defects.

**The structural invariant holds again: `rflags slot = rsp + 0x30` in all four lanes** —
now **eight fires across three binaries**, structural because it is what `this_cpu`'s
`push rbp` writes with `rbp` set by `finish_task_switch`'s prologue.

## §5 — LANE 12 IS A DIFFERENT PRODUCER AND IS DELIBERATELY NOT POOLED

`rip=0xFFFFFFFF8000CAF7` = **`isr_dispatch+0xfe7`**, `bt[0]` = `isr_common.gs_kernel_in+0x8`,
with `NEXUS KERNEL PANIC`, `panic_stage=3`, `panics_silent=0`, `SIGNAL x40`.
Disassembled in this binary:

```
ffffffff8000caf0: callq  <kputs>
ffffffff8000caf5: cli
ffffffff8000caf6: hlt
ffffffff8000caf7: jmp    0xffffffff8000caf5
```

That is **DDR-1099's fifth `[apfreeze]` producer**: the **winner's terminal halt at the end
of a COMPLETED panic report** — the `kputs` immediately before it is the `"halting."`.
`panic_stage=3` (a CPU claimed the latch) with `panics_silent=0` (nobody lost the CAS)
agrees: exactly one CPU panicked and nothing re-entered. **Not a `[schedcheck]` halt, not a
scheduler defect, and NOT POOLED** with the other three — pooling distinct producers into
one OPEN-2 rate is the conflation DDR-1019 exists to prevent, and §INV.18 is why the offset
matching DDR-1088's shard-3 freeze does not identify it: that was a *different binary*.

**Its NO-CHURN is a CONSEQUENCE, not a vacuous run** (DDR-1128's and DDR-1133 §9.4's point):
the panic lands at capture line 215, before the workload, and `churn_runs=49` confirms the
other 49 runs churned.

**The cause of that panic is NOT named and is NOT guessed — because the report that would
have named it is not in the job log. §6 is why.**

## §6 — THE HUNT'S OWN PRINTER DISCARDS THE PANIC REPORT (DESIGNED, NOT SHIPPED)

Lane 12's banner is at capture line **215**; the next line the job log shows is **241**.
`component:`, `exception:`, `vector=`, `RIP=`, `CR2`, `backtrace`, `halting.` — **every one
counts ZERO in the job log.** The ~25-line register block is gone.

`tools/ci/open2_hunt_campaign.sh:141` is:

```sh
grep -nE "$SIGNALS" "$cap" | head -40
```

**Matching lines only, no context in either direction.** A panic is written
**summary-first**, and its banner is the one line that carries no information, so the only
line that survives is the only one that says nothing.

**THIS IS THE DEFECT DDR-1088 FIXED, IN A THIRD PRINTER DDR-1088 DID NOT TOUCH.** DDR-1088
found that no CI job log had ever printed a frame of the panic report, measured the block
at 33 lines on a real capture, and fixed `check_global_forbidden` **and**
`scan_forbidden.sh` in one commit *precisely because two copies of one printer drift*. The
hunt has a **third** copy. DDR-1129 later raised its cap from `head -5` to `head -40`
without giving it context — a correct fix for the problem DDR-1129 had (four `[apfreeze]`
shots truncated), which did not reach this one. **Lane 12 is the first time it has cost
anything.**

**Two further measurements at the site, rather than an argument.** (a) `grep -c` reported
**40** and the cap is **40** and exactly **40** lines printed — so **nothing was truncated,
with zero margin**; one more matching line and the cap would have silently eaten it, with
`sig=41` against 40 printed as the only tell. (b) **35 of those 40 slots went to `[hb]`
heartbeat lines**, which match because `panic_stage=` is a heartbeat **field** (DDR-1049
put it there deliberately, and correctly). So the printer spends ~87% of its budget on
lines that are not events, and discards the one block that is.

**NOT SHIPPED (NON-NEGOTIABLE 5).** The remedy's shape is recorded so it is not re-derived:
print leading **and trailing** context per match, as DDR-1088 did, rather than re-ranking
the pattern list — DDR-1079 refused a hand-written cause/symptom order as *"one more list
to keep in step"* and that refusal stands, since printing both directions needs no
per-pattern knowledge. **A vacuity check is owed before any arm is written** and is stated
now: *"assert the panic body appears"* passes on any capture whose panic happens to sit
inside context already printed for some earlier match, which is exactly the trap DDR-1088
had to build its fixture around (banner, 60 filler lines, then a first match that takes the
context block).

**Unlike DDR-1132's remedy, this one is fixable from here**: DDR-1127 established the
harness lives on `dev/phase1-seyp3n`, so `open2_hunt_campaign.sh` is on **this** branch and
the default-branch constraint that blocked DDR-1132 §6 does not apply. It still needs its
own DDR, its own fixture, and a measured before/after.

## §7 — COUNTS, AND NO POOLED RATE

This dispatch is 20 lanes × 50 runs on one binary with **4 signal runs of two different
signatures** (three `[schedcheck]`, one completed panic). Across the three dispatches on
`ca8107ec7f5d8de7` the `[schedcheck]` check has now fired **six** times.

**NO POOLED RATE IS CLAIMED AND DDR-1132 §5's FIGURE IS NOT REVISED.** §4.3 shows the six
fires are not demonstrably the same event *as each other*, and §5's lane 12 is a different
producer again; revising a rate requires deciding which events are the same event, which is
precisely the decision this data cannot make. Doing the arithmetic anyway would be
DDR-1042's failure mode — a plausible number with nothing under it.

## §8 — NOT CLAIMED

- **NO FIX, NO MECHANISM NAMED, OPEN-2 DOES NOT CLOSE**, no open issue moves
  (OPEN-1/2/12/13 untouched). NON-NEGOTIABLE 3 undisturbed: §4 names a **co-occurrence**,
  and §4.2 states its size.
- **NO DEFECT ALLEGED** in `switch_wait_offcpu_sched`, `rq_push`, `rq_take`, the `on_cpu`
  handshake, `finish_task_switch`, `context_switch`, `reaper_thread`, `sched_exit`,
  `isr_dispatch`, the campaign script or the workflow. §6 corrects a **printer**, and the
  `[hb]` lines it matches on are DDR-1049's deliberate design working as intended.
- **DDR-1131 §2 IS NOT REFUTED AND IS NOT WITHDRAWN.** Its reading of the bail path stands;
  what changed is that the candidate it closed by reading now carries a measured
  co-occurrence, which is what DDR-1133 §5 asked for.
- **DDR-1118 IS NOT REINSTATED** — its mechanism was refuted on DDR-1120's artefact, and a
  second `saves+2` *without* its stated precondition does not supply it.
- **DDR-1133 §10.3's reading (ii) IS NOT REFUTED** and is not withdrawn; §4.2 states only
  the direction the correlation points.
- **DDR-1088 IS NOT CRITICISED** — its fix is correct and is what makes §6 legible as a
  *third* copy rather than a regression; **DDR-1129 is not criticised**, its cap raise being
  the right fix for the problem it had.
- **NO code change, NO build in the shipping tree, NO gate, NO new sentinel.**
  `GLOBAL_FORBIDDEN` **77**, **179 gates**, `kernel.bin` unchanged at `25f4dae4a3f90bcb`,
  1,319,306 B — so the size/headroom pair and `ci-docstate-check` are unaffected.
- **The `a390eab` worktree is a MEASUREMENT AID, NOT A RELEASE ARTEFACT** (DDR-1119's rule)
  and was removed; the shipping tree's `kernel.bin` was verified byte-identical before and
  after.
- **NO artifact was fetched and the proxy was not routed around.** The lane captures live
  in per-lane artifacts; `/root/.ccr/README.md` says to report the blocked host rather than
  retry, and everything above was read from the **job logs**.

---

**CORRECTED AT THE SITE 2026-09-23 — DDR-1136 §3.2.** §3's co-occurrence (every saves+2 fire
carries `r15 = …16842`) held at n=2 and **does not hold at n=6**: hunt run 35643638290 lanes
14 and 5 are saves+2 fires with `r15 = …1668D` (`finish_task_switch+0xd`). The datum that
pointed away from DDR-1133 §10.3 reading (ii) is gone; nothing is established in its place.
Also DDR-1136 §3.3: the `rflags = rsp + 0x30` "structural invariant" broke on lane 11
(`rsp + 0x1C0`), so it is not universal. This DDR is not withdrawn; the text above is left
as written so the record shows what was believed and when (DDR-1110).
