# DDR-1133 — THE FOURTH `[schedcheck]` FIRE CARRIES `disp = saves + 2`, THE VALUE DDR-1131 PRE-REGISTERED — AND ITS STATED PRECONDITION IS ABSENT

**Measurement + address resolution. NO CODE CHANGE, no gate, `kernel.bin` NOT
rebuilt in the shipping tree. NO FIX, NO MECHANISM NAMED, OPEN-2 DOES NOT CLOSE,
no open issue moves.**

---

## 0. The artefact

Run [35587697260](https://github.com/prady4the4bady/Prady4OS/actions/runs/35587697260),
`workflow_dispatch`, `lanes=20 runs=20 hunt=32`, ref `dev/phase1-seyp3n` at
`d280b80`. **2 of 21 jobs non-success: lanes 3 and 4.**

**DDR-1132's discriminator was applied FIRST and it passes.** Two lanes, not
twenty. That rule exists because this workflow's polarity is inverted
(`conclusion=failure` means *it found something*) and because my own dispatch the
previous day returned 20-of-20 failure from an `actions/checkout` refspec miss
that produced **zero boots**. A rare intermittent cannot fail every lane; a setup
failure cannot fail only two. Both lanes report churn (`churn_runs=20` and `19`),
so the boots were non-vacuous and DDR-1130's no-filesystem trap is not in play.

Lane 3, run 12:

```
[campaign] run=12 rc=1 [hb] t=18000 churn *** SIGNAL x5 *** cap=run-12.log.fail-6904
[schedcheck] next->rsp invalid tid=22 pid=22 rsp=0x0000000007D03A30 base=0x0000000007D00000
             rflags=0x0000000007D03A60 r15=0xFFFFFFFF80016842 ret=0xFFFFFFFF80016DCF
             rq_on=0 disp=38 saves=36 halting.
[apfreeze] cpu=2 ticks=148 rip=0xFFFFFFFF80016D91 if=0 rflags=0x02 masked=0 svr=0x1FF
           swen=1 tpr=0 isr48=0 irr48=1 pid=22 shot=1..4
           bt=0xFFFFFFFF80013C01,0xFFFFFFFF80015857,0xFFFFFFFF8001A7C5,0xFFFFFFFF8001A401
           ticks[0=1500,1=1479,2=148,3=1476] ... [0=3000,1=2979,2=148,3=2976]
[campaign] DONE runs=20 signal_runs=1 churn_runs=20 kernel_pinned=ca8107ec7f5d8de7
```

**§INV.18 satisfied by rebuild, not by assumption.** `a390eab` built with
`OPEN2_HUNT=32` reproduces `ca8107ec7f5d8de7` **bit-for-bit**, 1,319,306 B,
warning-clean at `-Werror`, built in a detached worktree so the shipping tree's
`build/` was never touched (DDR-1119's discipline; DDR-1060 §9's void-campaign
hazard avoided rather than rediscovered). Every address below is resolved against
**the binary that produced the capture**, in `python3`, never `awk`
(DDR-1079/DDR-1121). **`nasm` is present in this container now** — the blocker
DDR-1129 §7 recorded is gone, and that is why this fire could be resolved at all.

---

## 1. Address resolution

| field | address | resolves to |
|---|---|---|
| `[apfreeze] rip` | `0xFFFFFFFF80016D91` | `schedule_locked+0x671` |
| `bt[0]` | `0xFFFFFFFF80013C01` | `schedule+0x11` |
| `bt[1]` | `0xFFFFFFFF80015857` | `yield+0xe7` |
| `bt[2]` | `0xFFFFFFFF8001A7C5` | `sys_yield+0x25` |
| `bt[3]` | `0xFFFFFFFF8001A401` | `syscall_dispatch+0x131` |
| `[schedcheck] r15` | `0xFFFFFFFF80016842` | `schedule_locked+0x122` |
| `[schedcheck] ret` | `0xFFFFFFFF80016DCF` | `schedule_locked+0x6af` |

**The `rip` is byte-identical to DDR-1128/1129's, on the same binary** — the
deliberate `hlt` at `sched.c:1857`, reachable only from inside DDR-1105's
`next->rsp` validity check. DDR-1129's resolution carries by identity, not by
offset-matching (§INV.18's trap, avoided).

**THE ARRIVAL PATH IS NEW AND IT GENERALISES DDR-1129.** DDR-1128's backtrace was
`schedule+0x11 ← sched_ap_enter+0x178 ← smp_ap_entry+0x30A`, the **AP bring-up**
path. This one is `schedule+0x11 ← yield+0xe7 ← sys_yield+0x25 ←
syscall_dispatch+0x131` — an ordinary **ring-3 `SYS_YIELD`**, consistent with
`pid=22`. Same halt site, two unrelated arrival paths. DDR-1129's finding (the
`[apfreeze]` is a *deliberate* halt downstream of the check, not a spin) is
therefore a property of the halt site rather than of the AP path, which is a
strengthening of that DDR and not a correction to it.

---

## 2. The frame: two of three witnesses match the consumed-frame chain, one does not

The chain, disassembled **in this binary** rather than carried from DDR-1120's:

```
ffffffff80016dbc:  callq <context_switch>        ← the kernel's ONLY one (verified: 1 match)
ffffffff80016dc1:  callq <finish_task_switch>
ffffffff80016dc6:  movq  -0x8(%rbp), %rdi
ffffffff80016dca:  callq <local_irq_restore>
ffffffff80016dcf:  addq  $0x1b0, %rsp

ffffffff80016680 <finish_task_switch>:   push rbp / mov rsp,rbp / sub $0x20,rsp
ffffffff80016688:  callq <this_cpu>       → returns 0xffffffff8001668d  (= +0xd)
```

`finish_task_switch` is **out of line** in this binary (symbol at `0x16680`),
checked rather than assumed — DDR-1119 records that an inlined build would break
the prediction outright.

Resuming from a frame at `S` (context.asm's 8 quadwords: RFLAGS, r15, r14, r13,
r12, rbp, rbx, return address at `S+0x00 … S+0x38`) leaves:

| slot | consumed-chain prediction | observed | |
|---|---|---|---|
| `[S+0x00]` rflags | `S+0x30` (`this_cpu`'s `push rbp`) | `0x07D03A60` = `rsp+0x30` | **MATCH** |
| `[S+0x08]` r15 | `finish_task_switch+0xd` = `0x1668d` | `0x16842` | **MISMATCH** |
| `[S+0x38]` ret | post-`call local_irq_restore` = `0x16dcf` | `0x16dcf` | **MATCH** |

**It is not a fresh save.** A genuine `context_switch` save carries the return
address `call context_switch` pushed, i.e. `0x16dc1`. The slot reads `0x16dcf`,
eight bytes of instruction stream later and four instructions on. So the frame is
spent, exactly as DDR-1118/1119/1120 established for fires 1–3.

**AND THE MISMATCHED SLOT HOLDS ONE SPECIFIC RETURN ADDRESS:**

```
ffffffff8001683d:  callq <switch_wait_offcpu_sched>
ffffffff80016842:  cmpl  $0x0, %eax          ← observed r15, exactly
ffffffff80016845:  jne   <schedule_locked+0x17c>
```

`r15` is the return address of `call switch_wait_offcpu_sched`. For that value to
sit at `S+0x08` after the consume, something executed that call with `rsp =
S+0x10` — i.e. a **later, deeper** activation on this same stack overwrote the
`this_cpu` litter while leaving `S+0x00` and `S+0x38` intact.

**THE STRUCTURAL INVARIANT HOLDS A FOURTH TIME:** `rflags` slot = `rsp + 0x30`,
and the frame sits **1488 B** below ktop against DDR-1115's 1472 and DDR-1118's
1520 — the same call chain within 48 bytes, which is DDR-1119's observation
reproduced. `tid=22 pid=22` matches fires 1 and 2, and is **not** leaned on:
DDR-1119 already recorded tid 22 as a weak narrowing, `t->tid = next_tid++` being
boot-order.

---

## 3. `disp = saves + 2` — the pre-registered value, without its pre-registered corroborator

`disp=38 saves=36`. DDR-1118 §6 defined this in advance — *"disp == saves + 1 on
EVERY healthy switch, and disp == saves + 2 is a thread switched in twice with no
save between"* — and DDR-1131 §3 pre-registered it, **before this data existed**,
as *"DOUBLE RESUME CONFIRMED, DDR-1118's mechanism named."* Fires 1–3 all read
`saves + 1`; DDR-1120 refuted DDR-1118's mechanism on precisely that number.
**This is the first time in the project's history that the identity does not
hold.**

**AND THE ROW'S COROBORATOR IS ABSENT, WHICH IS NOT A DETAIL.** DDR-1131 §3 reads
*"with rq_on=1 corroborating via its stated precondition"*, and `rq_on=0`.
DDR-1118 built `rq_on` precisely because a double resume through the runqueue
needs the thread queued while running. **The pre-registration contains a row for
the mirror case** (precondition without consequence → *"a NARROWING NOT A
MECHANISM"*) **and no row for this combination**, because it assumed the two
would travel together. Forcing this into the nearest row is exactly the failure
DDR-1128 §3 committed and DDR-1042 is the standing record of. It is not done
here.

### 3.1 `saves + 2` does not uniquely name a double resume — and the second reading is the one that matters

DDR-1120 §5(b) recorded that the identity is **blind to a recycled TCB**:
`kmalloc` does not zero (NON-NEGOTIABLE 10), so a reissued TCB carries its
previous owner's counters, and a **parked** previous owner satisfies `old_disp ==
old_saves`, making `disp = saves + 1` the *healthy* identity on a recycled object.

**Run the same arithmetic on a RUNNING previous owner.** A thread that has been
dispatched and not yet switched away satisfies `old_disp == old_saves + 1`. Reissue
that TCB, dispatch once under the claim, and the check reads **`disp = saves +
2`** — the observed value, with no double resume anywhere.

So one number carries two readings:

- **(a) a genuine double resume**, DDR-1118's mechanism, whose stated precondition
  (`rq_on=1`) is **absent here**;
- **(b) a TCB reissued from a RUNNING owner**, DDR-1096 §3's hypothesis, which
  requires no double resume at all and for which `rq_on=0` is unsurprising.

**This capture does not discriminate them, and neither does the `[ringwalk]`
absence** — see §4.

---

## 4. No `[ringwalk] RECYCLED`, and the absence is real but weaker than it looks

Checked in the script rather than inferred from the output:
`open2_hunt_campaign.sh:101` has `\[ringwalk\]` inside `SIGNALS`; the print cap at
`:141` is `head -40` (DDR-1129's designed fix shipped, which is also why
`[schedcheck]` reaches the job log at all and why DDR-1129 §7's artifact was not
needed to read this fire); and `sig` at `:132` is a **`grep -c` total**. The lane
reported `*** SIGNAL x5 ***` and printed five lines. **Five matched, five printed,
nothing truncated.**

So DDR-1131 §3's load-bearing co-occurrence row **does not fire**, and the
recycled-TCB reading is **not named** by this capture.

**IT IS ALSO NOT EXCLUDED, AND THAT ASYMMETRY IS STATED RATHER THAN GLOSSED.**
The `[ringwalk] RECYCLED` detector is not a general recycle detector: it is a
`tid` re-read across an `OPEN2_HUNT` pause inside `sched_tick`'s DDR-955
all-threads sweep (`sched.c:~2005`). It fires when the sweep happens to be
holding a node that is reissued during its own pause window. A TCB reissued
anywhere else, at any other moment, produces no `[ringwalk]` line at all.
**Absence of the precondition detector is evidence about the detector's window,
not about recycling** — DDR-1127 §4 already measured that this detector fired
zero times in 60 boots, and DDR-1127 §3 reading (3) left its sensitivity
explicitly untested.

---

## 5. What this does to DDR-1131 §2 — which is NOT withdrawn

DDR-1119 named `switch_wait_offcpu_sched`'s bounded handshake as *"where a THIRD
fire's `rq_on=` and `disp-saves` will point, or away from."* DDR-1131 §2 then
**closed that candidate by reading**: the bail path `rq_push`es the contended
thread back and substitutes `g_idle[cpu]`, so a `dispatches++` past it belongs to
idle, and **the bail path cannot produce a double resume.**

That reading is re-read here in full (`sched.c:700-724`, call site `:1555`) and
**it is correct as far as it goes** — `return 0` is preceded by
`g_wait_bails[cpu]++` and `g_in_switch[cpu] = 0`, and the caller at `:1555`
declines the pick. **DDR-1131 §2 IS NOT REFUTED AND IS NOT WITHDRAWN.**

What changes is narrower and is the reason this section exists: **the fourth fire
puts that function's return address in the frame**, alongside the first
`disp = saves + 2` ever observed. A value in a spent frame is **litter** — it says
something executed there, not that it is the defect (DDR-1056: a matching shape is
not a mechanism), and this DDR alleges no defect in `switch_wait_offcpu_sched`,
`rq_push`, `rq_take` or the `on_cpu` handshake. But a candidate closed on a
*reading* now has an *artefact* touching it, and the honest disposition is
**re-open for measurement, not re-close on the same reading.**

### 5.1 The one measurement that would separate §3's two readings, OWED and BLOCKED

`g_wait_calls[]` and `g_wait_bails[]` are already maintained and already printed —
as `calls=` and `bails=` in the `[hb]` heartbeat. **Lane 3's heartbeats are not in
the job log**, because nothing in lane 3's capture matched `SIGNALS` on a
heartbeat line (lane 4's heartbeats *are* in its log, only because they carry
`panic_stage=`). They exist in artifact `open2-hunt-lane-3` (ID **10636246167**,
retention 14 days).

**That artifact cannot be fetched from this container.** The egress proxy refuses
`productionresultssa*.blob.core.windows.net` with `connect_rejected`, and
`/root/.ccr/README.md` §*"403 / 407 from the proxy"* says plainly *"Do not retry
or route around it — report the blocked host."* **Reported here, not routed
around** — the same disposition DDR-1129 §7 took, and the same standing operator
instruction.

**DESIGNED, NOT SHIPPED (NON-NEGOTIABLE 5).** The `[schedcheck]` line is emitted
at the halt and already prints eleven fields; adding `bails=` and `calls=` for the
halting CPU would answer §3 from the **job log** on the next fire, with no
artifact fetch and no new gate. It is a one-line addition to an already-cold
failure path (DDR-1123's cost argument applies unchanged: the print arm runs only
after a CPU has frozen). **The line-length budget must be recomputed at the site
before it ships** — DDR-1118 §8.1 found this exact line at 251 bytes against a
usable 254, and `[kline] TRUNC` is in `GLOBAL_FORBIDDEN` and would destroy the
artefact the line exists to carry. Two `kline_d` fields do not fit. **So the
shipped form would have to replace or abbreviate, not append**, and that is a
design decision with its own DDR, not a change to make in passing.

---

## 6. Lane 4 is a second signal and a DIFFERENT producer — deliberately not pooled

`signal_runs=1 churn_runs=19`, heartbeats carrying `panics_silent=1 panic_stage=3
loser_cpu=3 loser_vec=13 loser_rip=0xFFFFFFFF80012A91`.

That is a **silent panic** — DDR-1049's detector, which exists precisely so a
panic whose winner never reached its banner names itself instead of passing. It is
**not** a `[schedcheck]` halt. `loser_vec=13` is a `#GP`, which on a plain load
means a **non-canonical address** (DDR-1079's reasoning).

`loser_rip` resolves to **`resolve+0x61`**, and the symbol is unambiguous —
`llvm-nm` returns exactly one `resolve`, at `0xffffffff80012a30`, which is
`kernel/cap.c:42`'s `static struct cap_slot *resolve(struct cap_table *t, cap_t
h)`. **DDR-1121's `g_rq` aliasing trap was checked, not assumed away.**

**NOT ANALYSED FURTHER HERE AND NOT ATTRIBUTED.** It is a distinct signature with
a distinct producer, and pooling it with lane 3 — or with DDR-1128's `[apfreeze]`
— into one "OPEN-2 rate" is exactly the conflation DDR-1019 exists to prevent
(`[apfreeze]` alone has at least five producers). It is recorded so the next
session starts from the resolved address rather than from the hex.

---

## 7. What the counts are, and what they are not

This run: **20 lanes × 20 runs = 400 boots**, one binary `ca8107ec7f5d8de7`, two
signal runs of **two different signatures**.

**NO POOLED RATE IS CLAIMED AND DDR-1132 §5's FIGURE IS NOT REVISED HERE.** That
figure (1 in 460, exact 95% CI [0.0055%, 1.205%]) was computed over
`[apfreeze]`-class events on this binary. Adding lane 3 to it is defensible;
adding lane 4 is not, because it is a different producer. Revising a rate requires
deciding which events are the same event, and that decision is exactly what §3
shows this capture cannot make. **Doing the arithmetic anyway would be DDR-1042's
failure mode — a plausible number with nothing under it.**

What *is* new and is stated without a denominator: **`disp − saves ≠ 1` has now
been observed, once, ever.**

---

## 8. NOT CLAIMED

- **NO FIX. NO MECHANISM NAMED. OPEN-2 DOES NOT CLOSE**, and no open issue moves
  (OPEN-1/2/12/13 untouched). NON-NEGOTIABLE 3 is undisturbed: `disp = saves + 2`
  names a **number**, and §3.1 shows the number carries two readings.
- **NO code change, NO gate, NO new sentinel** (`GLOBAL_FORBIDDEN` 77), 179 gates,
  `kernel.bin` NOT rebuilt in the shipping tree, so the size/headroom pair and
  `ci-docstate-check` are unaffected. The `a390eab` worktree is a **measurement
  aid, not a release artefact** (DDR-1119's rule) and is removed afterwards.
- **DDR-1131 IS NOT WITHDRAWN.** Its §3 table is pre-registration and did its job:
  it named `saves + 2` in advance, which is why this fire could be read against a
  fixed standard instead of a convenient one. §3 of this DDR records that the
  observed *combination* was not in the table — that is the pre-registration
  meeting reality, not failing.
- **DDR-1131 §2 IS NOT REFUTED** (§5). Its bail-path reading is re-read and holds;
  what is recorded is that the candidate now has an artefact touching it.
- **DDR-1118 IS NOT REINSTATED.** Its mechanism was refuted on DDR-1120's
  artefact; this capture supplies the consequence without the precondition, which
  is not the same as supplying the mechanism.
- **DDR-1120, DDR-1128, DDR-1129 are NOT withdrawn or corrected.** DDR-1129 is
  *strengthened* (§1): its halt-site attribution now holds across two unrelated
  arrival paths.
- **NO defect is alleged** in `switch_wait_offcpu_sched`, `finish_task_switch`,
  `context_switch`, `rq_push`, `rq_take`, the `on_cpu` handshake, `cap.c`'s
  `resolve`, the campaign script, or the workflow.
- **NO RATE** (§7). **Lane 4 NOT attributed** (§6). **Lane 3's `calls=`/`bails=`
  values OWED and BLOCKED, not routed around** (§5.1).

---

## 9. The second 400-boot dispatch — two more fires, and they supply the control §3 lacked

Run [35587705666](https://github.com/prady4the4bady/Prady4OS/actions/runs/35587705666),
same parameters, **same pinned binary `ca8107ec7f5d8de7`**, **2 of 21 jobs
non-success** (lanes 5 and 13), so DDR-1132's discriminator passes here too.

```
lane 5  run 15  churn_runs=20
[schedcheck] tid=11 pid=0  rsp=0x07C32578 base=0x07C30000 rflags=0x07C325A8
             r15=0x0  ret=0xFFFFFFFF80016DC1  rq_on=0 disp=36777 saves=36776
  + 4x [apfreeze] cpu=3 ticks=588 rip=0xFFFFFFFF80016D91, ticks[0=2000,1=1979,2=1978,3=588]

lane 13 run 10  churn_runs=19   [hb] t=1000 NO-CHURN, SIGNAL x1
[schedcheck] tid=22 pid=22 rsp=0x07D03690 base=0x07D00000 rflags=0x07D036C0
             r15=0x10 ret=0x0  rq_on=0 disp=8 saves=7
```

**Both read `disp = saves + 1`, `rq_on=0`, no `[ringwalk]`. That is DDR-1131 §3's
row verbatim — *"DDR-1120 REPRODUCED, NOTHING NEW, record it and say so."*
Recorded, and said.**

### 9.1 This is the control §3 could not have on its own

A single `disp = saves + 2` is consistent with the counters simply being read
wrongly — a mis-ordered increment, a wrong `rbp` offset, an instrument defect.
**Lane 5 and lane 13 remove that reading by measurement rather than by argument:**
the *same* code, in the *same* binary, in the *same* workflow, hours apart, read
the healthy identity **twice**. The instrument is not systematically off by one.

**Lane 3's `+2` is therefore an observation about that boot, not about the
instrument.** This is the check DDR-1126 §6 insists on in its own words — without
a run where the thing does *not* happen, *"the check catches it"* and *"the check
was always going to pass"* are the same observation.

### 9.2 Three `ret` classes on ONE binary — the family is not one state

DDR-1116 built the `ret` field precisely because its legal set has **exactly two**
members, so it discriminates *(A) a real frame whose content was overwritten* from
*(B) a pointer that addresses no frame at all*. Across three fires on one binary it
takes **three different classes of value**:

| fire | `ret` | class |
|---|---|---|
| lane 3 | `0xFFFFFFFF80016DCF` | post-`call local_irq_restore` — **consumed-frame litter** |
| lane 5 | `0xFFFFFFFF80016DC1` | post-`call context_switch` — **a LEGAL value**, what a fresh save carries |
| lane 13 | `0x0` | **neither** legal value — DDR-1116's hypothesis (B) |

**And lane 5 is internally mixed**, which is the sharpest single fact in this
section: its `ret` slot holds exactly what `call context_switch` pushes, i.e. the
return slot of a **fresh** save — while its `rflags` slot holds `rsp+0x30`, a
**stack address**, where a fresh save carries what `pushfq` pushed. One frame, one
slot saying *fresh*, another saying *consumed*.

**So `[schedcheck]` is catching at least three distinguishable frame states, and
they have been read as one signature.** That is DDR-1019's finding one level in:
there, `[apfreeze]` turned out to have at least five producers told apart only by
RIP; here the `[schedcheck]` family is told apart by the `ret` slot, and nobody
had three fires on one binary to compare until now. **No mechanism is named for
any of the three**, and specifically it is **not** claimed that they are the same
defect or that they are different defects — only that the states differ and that
pooling them is now a choice rather than a default.

### 9.3 The invariant that survives all six fires

`rflags` slot `= rsp + 0x30` holds in **every one**: DDR-1115, DDR-1118, DDR-1120,
and all three here — **six fires across three binaries**. It is the single most
robust fact about this family, and DDR-1118 explained why it is structural rather
than coincidental: it is what `this_cpu`'s `push rbp` writes, with `rbp` set by
`finish_task_switch`'s own prologue. Nothing here disturbs that.

### 9.4 Lane 13's NO-CHURN is a consequence, not a vacuous run

`[hb] t=1000 NO-CHURN` and `SIGNAL x1`: the boot halted at tick 1000, long before
`rqstress_proof`, **because the check halted the CPU**. DDR-1128 made exactly this
point about its own capture, and the lane's `churn_runs=19` confirms the other
nineteen runs did churn. **DDR-1130's trap is not in play** and this run is not
counted as a vacuous clean — it is counted as a fire. `disp=8` also says the
thread had been dispatched only eight times, so whatever selects a bad `next->rsp`
is **not** confined to long-lived threads — lane 5's `disp=36777` on a kernel
thread is the same family at four orders of magnitude more churn.

### 9.5 Counts, and again no pooled rate

**Two completed dispatches: 800 boots on one binary, 4 signal runs — three
`[schedcheck]` and one silent panic.** The `[schedcheck]` fires are 3 in 800.

**NO RATE IS CLAIMED AND DDR-1132 §5's FIGURE IS STILL NOT REVISED.** §7's reason
stands and is now sharper, not weaker: §9.2 shows the three `[schedcheck]` fires
are not demonstrably the same event as each other, let alone the same event as
DDR-1128's `[apfreeze]`. Computing one rate over them would require the very
decision this DDR records as unmade. **Doing the arithmetic anyway would be
DDR-1042's failure mode.**

### 9.6 Added to NOT CLAIMED

- **Lanes 5 and 13 name NO mechanism** and are recorded as DDR-1120 reproduced,
  exactly as DDR-1131 §3 pre-registered.
- **§9.2 alleges no defect** in `[schedcheck]`, in DDR-1116's `ret` field, or in
  the decision to treat these fires as one family up to now — the three-state
  observation was **not available** before three fires existed on one binary.
- **§9.1 does not validate lane 3's `+2` as a mechanism.** It removes one
  alternative explanation (a systematically miscounting instrument). §3.1's two
  readings both remain open and undiscriminated.

---

## 10. CORRECTION TO §3.1, MADE BEFORE THIS SHIPPED — the enumeration was short by the most economical reading, and it cuts against §3's headline

§3.1 offered **two** readings of `disp = saves + 2`. Checking them in the source
rather than leaving them as prose found that one is **wrong as written** and that
**two more exist**, one of which makes the number instrument noise.

### 10.1 §3.1's reading (b) is refuted for the ordinary reissue path

§3.1 said a TCB *"reissued from a RUNNING owner (`old_disp == old_saves + 1`) plus
one dispatch"* yields `saves + 2`. **That requires the counters to survive the
reissue, and they do not.** Measured:

- `sched.c:1092` — `t->switches_away = 0;  /* DDR-1118; NON-NEGOTIABLE 10 */`
- `sched.c:1173` — `t->dispatches = 0;`
- **Both are inside `sched_create_state` (declared `:1074`)**, the single creation
  path: `sched_create` (`:1209`), `sched_create_blocked` (`:1222`),
  `sched_create_user` (`:1243`) and `sched_create_user_clone` (`:1266`) all call
  it, and the only other `kmalloc(sizeof(struct tcb))` is the AP idle (`:1030`),
  which is `memset` to zero wholesale (`:906`).

**So a reissued TCB reads `0/0` and its first dispatch gives `disp = saves + 1` —
the healthy identity — on every path.** Reading (b) survives only under a much
narrower precondition I did not state: a reissue that **races**
`sched_create_state`'s initialisation. That is a strictly stronger claim than the
one §3.1 made, and §3.1 is wrong to that extent.

**No NON-NEGOTIABLE 10 violation is found**, and that is recorded rather than
passed over — an audit that reports only errors is not an audit. **DDR-1118's
claim that `switches_away` gets an explicit initialiser is CORRECT**; a first
grep here appeared to show none, and *the grep was what was wrong* (its
comment-stripping filter dropped the very line that carries the
`/* DDR-1118; NON-NEGOTIABLE 10 */` note). Recorded because the same filter will
lie the same way next time.

### 10.2 DDR-1120 §5(b)'s conclusion survives; its stated reason does not reach

DDR-1120 §5(b) reasoned that *"a reissued TCB carries its PREVIOUS OWNER's
counters"*. That is true of `kmalloc` handing back dirty memory and **not** true
of a live thread, because `sched_create_state` zeroes both before it runs. Its
**conclusion** — `saves + 1` on a recycled object — is nonetheless right, by a
different route (a zeroed object's first dispatch reads `1/0`). **DDR-1120 is not
withdrawn; one clause of its reasoning is narrowed.**

### 10.3 The reading §3.1 missed, and it is the most economical one

`kernel/proc/sched.h:210-226` states it outright, and this DDR should have read it
before §3 was written:

> *"PRINTED, NOT JUDGED: **the increments are plain (non-atomic)** and `sched_exit`
> leaves by a path that does not pass the save site, so **the identity holds by
> the code's habits and NOT by construction** — exactly the test DDR-1116 refused
> to promote into a clause on the hottest path in the kernel."*

`prev->switches_away++` (`sched.c:1866`) is a **non-atomic read-modify-write**,
sitting immediately before the kernel's only `context_switch(&prev->rsp,
next->rsp)` (`:1867`). **If two CPUs increment the same TCB's `switches_away`
concurrently, one increment is lost**, that thread's `saves` is permanently one
low, and its next dispatch reads `disp = saves + 2` — **with no double resume
anywhere, and with `rq_on=0` entirely unsurprising, because nothing about a lost
increment requires the thread to be queued.**

Concurrent switch-away on one TCB is not hypothetical here: it is what the rq-2
work-stealing design permits around the `on_cpu` handshake, which is the region
`sched.c:202-207` describes and which DDR-1119 flagged.

**So the full enumeration of what produces `saves + 2` is four, not two:**

| | reading | status |
|---|---|---|
| (i) | genuine double resume (DDR-1118's mechanism) | precondition `rq_on=1` **absent** |
| (ii) | **a LOST non-atomic `switches_away++`** | needs **no defect** beyond documented non-atomicity |
| (iii) | a switch-away bypassing the single save site | `sched_exit` is the documented one, but an exiting thread is not dispatched again |
| (iv) | a reissue **racing** `sched_create_state` | §3.1's (b), narrowed by §10.1 |

### 10.4 What this does to §3's headline — stated plainly, because it cuts against it

**If (ii) is what happened, `disp = saves + 2` says nothing about the frame at
all.** It would be a race in the *instrument*, not evidence about the defect the
instrument was watching — and DDR-1131 §3's pre-registered row would have been
reading counter noise as a mechanism.

**This does not retract §3 and it does not promote (ii) either.** What is
established is that the pre-registered row's reading is **one of four**, that the
two the pre-registration leaned on ((i) and §3.1's (b)) are the two now in the
worst shape, and that **the enumeration, not the number, is what was missing.**
§9.1's control is untouched by this — it still shows the counters are not
*systematically* off by one — but a *lost* increment is by definition sporadic, so
§9.1 does not bear against (ii) at all.

**NOT SHIPPED, and the refusal is the same one twice made (NON-NEGOTIABLE 5).**
Making both increments atomic would turn the identity from habit into
construction and would separate (ii) from (i) on the next fire. It is **not** done
here: it is a change to the hottest path in the kernel, on the very path OPEN-2
lives in, which is the cost DDR-1047 refused and DDR-1116 and DDR-1118 each
declined for this exact field. It also **would not be a fix** — it would repair
the *instrument*, while the defect under investigation is the bad `next->rsp`. It
needs its own DDR, its cost measured, and a forced mutant.

### 10.5 Added to NOT CLAIMED

- **(ii) is NOT claimed to have happened.** It is an enumeration gap being closed,
  not an attribution. No mechanism is named and **OPEN-2 still does not close.**
- **NO defect is alleged** in `sched_create_state`, in the non-atomic increments
  (which `sched.h` documents as deliberate and which DDR-1116 deliberately
  declined to harden), or in DDR-1118's instrument.
- **DDR-1131 §3 is NOT withdrawn.** Pre-registration fixed a reading before the
  data, which is what let this correction be *recognised as a correction* rather
  than quietly adopted; §10.4 records that its row is one reading of four.
- **§3 and §3.1 are corrected here rather than rewritten** (DDR-1110's rule), so
  the record shows what was believed and when.

---

## 11. THE OWED MEASUREMENT, MADE — AND §5.1 OVERSTATED WHAT THE BLOCKED ARTEFACT COULD HAVE ANSWERED

§5.1 recorded two things as owed: recompute the line-length budget at the site
before the `bails=`/`calls=` design could be decided, and fetch artifact
**10636246167** for lane 3's heartbeats. **Both are settled here by reading, with
no code change, no build and no fetch** — and the second is settled in the
direction that makes my own reported loss *smaller*, which is why it is stated
plainly rather than left implicit.

### 11.1 The budget: §5.1's assertion is CONFIRMED, now by arithmetic

`g_wait_calls[]` and `g_wait_bails[]` are `volatile uint32_t` (`sched.c:692-693`),
so each is **10 digits** worst case on the type-based figure DDR-1116 requires.

| term | bytes |
|---|---|
| `" calls="` | 7 |
| value, `uint32_t` | 10 |
| `" bails="` | 7 |
| value, `uint32_t` | 10 |
| **append cost** | **34** |

The line stands at **251 against a usable 254** (recorded at the site; `kline_c`
truncates when `n + 1 >= KLINE_MAX`, `KLINE_MAX` 256, `console.h:23`). **Margin 3.
34 does not fit, over by 31.** §5.1 said two `kline_d` fields do not fit and was
right; it was an assertion then and it is a measurement now.

**Nothing in the current line is safely removable**, checked field by field
rather than assumed:

- the five `kline_x` fields (90 bytes) — shortening the hex form changes every
  recorded grep and every DDR that quotes an address verbatim;
- the literals (100 bytes) — the field names **are** the artefact's identity;
  §9.2's three-`ret`-class finding is stated in terms of them;
- `disp`/`saves` as one difference instead of two values (frees 20) — **refused**,
  because §9.4 is a finding that rests on the *absolute* values (`disp=8` against
  `disp=36777` is what showed the defect is not confined to long-lived threads).

### 11.2 The artefact would NOT have answered §3 — two independent reasons, both in source

**(a) The heartbeat DRAINS the counters.** `sched_take_wait_stats` (`sched.c:744`)
reads with `__atomic_exchange_n(&g_wait_calls[c], 0, __ATOMIC_RELAXED)` — a
read-and-clear. Its sibling `sched_take_spin_stats` states the consequence in its
own comment: *"then zeroes them — so each heartbeat line describes exactly one
500-tick window."* So `calls=`/`bails=` in `[hb]` are a **per-window** count, and
the last heartbeat before lane 3's halt describes a window the halt **truncated**.

**(b) It sums across all CPUs.** That function accumulates `for (int c = 0; c <
PERCPU_MAX; c++)` into one global pair. The `[schedcheck]` fire is on **one** CPU.
So even a complete window would not say whether the **halting** CPU bailed, which
is the question §3 actually needs answered.

**So the fetch was owed for a question it could not have settled.** The proxy
block cost less than §5.1 recorded. This is recorded despite being convenient
because it rests on two lines of source (the exchange-with-zero, and the
all-CPU sum), not on an argument — and because §5.1's claim is quoted in
CLAUDE.md and in the PR thread, where it would otherwise stand uncorrected.

### 11.3 It also makes the DESIGN better than the fetch — but NOT in the way it is tempting to say

At the halt site the available value is `g_wait_bails[cpu]` for the **halting
CPU**, which no heartbeat has ever printed. That is a real improvement over (b).

**It is NOT cumulative, and that must not be claimed.** The same tick-driven
drain clears the per-CPU slots, so a value read at the halt is *that CPU's bails
within the current heartbeat window*. For "did the bail path run near this fire"
a window ending at the halt is arguably the right window. For "has this CPU ever
bailed" it is useless — **and a ZERO would mean only "not in this window", never
"never"**, which is exactly the reading that would otherwise be taken as an
exoneration of the `switch_wait_offcpu_sched` candidate §5 re-opened.

### 11.4 The shape that fits the budget — NOT shipped, NOT verified

A **second `kline_emit`**. DDR-1055's hazard is a splice *within* a line built
from several `kputs`; two separate emits are each atomic on their own. What is
lost is *adjacency* — another CPU may emit between them — which a repeated `tid=`
re-keys. On a fresh 256-byte line ~40 bytes has enormous margin.

**Recorded as the shape that fits the BUDGET question only.** I have not read
`kline_emit` to confirm a second call is safe from this context (after `cli`,
immediately before the halt), and that is the load-bearing check, not the
arithmetic. **No kernel change is made**: the campaign is mid-flight on pinned
`ca8107ec7f5d8de7` and a rebuild would break the single-binary discipline the
whole dataset rests on. It needs its own DDR, its own forced mutant and its own
regression run.

### 11.5 NOT CLAIMED

- **NO code change, NO build, NO gate, `kernel.bin` NOT rebuilt.** GLOBAL_FORBIDDEN
  77, 179 gates. **No mechanism named; OPEN-2 does not close; no open issue moves.**
- **NO defect is alleged** in `sched_take_wait_stats`, `sched_take_spin_stats` or
  `switch_wait_offcpu_sched`. The drain is *correct and deliberate* — DDR-890 built
  it so each heartbeat describes one window, which is the right design for a
  heartbeat. What is corrected is **my reading of what those counters could tell a
  reader at a halt**, not the counters.
- **§5.1 is NOT withdrawn** and is corrected here rather than rewritten
  (DDR-1110's rule): its budget claim was right, its disposition on the proxy was
  right, and one clause — that the artefact holds the measurement that would
  separate §3's readings — is narrowed by §11.2.
- **DDR-1129 §7 item 1 is NOT thereby discharged.** That item is the
  `[schedcheck]` *values*, which DDR-1133 §1-§3 now has. This concerns only the
  `calls=`/`bails=` follow-up, and §11.3 says what a future field could and
  could not establish.

### 11.6 A provenance note, recorded because it caught a stale expectation of mine

Writing §11 I went to restate the standing pin and found **`build/kernel.bin`
absent from the shipping tree** (`build/` holding only logs, `gatelogs`,
`cr3fixtures` and the `musl`/`lwip` prerequisites; 30 GB free, so not disk
pressure). **What removed it is not established and is not guessed at.**

**No claim in this DDR rests on that file.** Every result here came from CI
captures and from the detached-worktree rebuild of §0, and §8 records that the
shipping tree was deliberately never built in.

**The value I was about to check against was itself two kernels stale.** I
expected `6af029b001e6e6db` — the pin from the DDR-1118/1121/1122 era — when
`git log -1 -- Makefile 'kernel/**' 'boot/**' 'user/**' 'arch/**'` names
**`a390eab` (DDR-1126)** as the last commit touching a build input, so the
correct value is **`25f4dae4a3f90bcb`**. Had the file been present I would have
read a correct hash as a moved binary. That is the DDR-1111 class — a dated
measurement carried forward as current state — arriving in my own working
memory rather than in a document.

**Rebuilt, and it reproduces:** `make image` from an empty `build/` gives
**`25f4dae4a3f90bcb`, 1,319,306 B, warning-clean at `-Werror`, rc=0**. That is
worth more than the pin it replaces:

- it **independently confirms DDR-1126's own recorded hash** on a from-scratch
  build rather than an incremental one, so §INV.10's don't-always-rebuild trap
  cannot be in play;
- it establishes that every commit from `a390eab` to `HEAD` is docs-only **by
  construction** — the binary is identical — rather than by reading
  `git diff --name-only`;
- and it confirms `tools/ci/open2_hunt_campaign.sh` (changed in that range) is
  **not a build input**, measured rather than assumed.

**Carry:** a hash pin is a dated measurement. Re-derive which commit last touched
a build input before comparing against one, or a correct binary reads as a moved
one.
