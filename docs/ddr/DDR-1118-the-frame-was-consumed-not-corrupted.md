# DDR-1118 — THE FRAME AT `next->rsp` WAS **CONSUMED**, NOT CORRUPTED

**Status:** artefact resolved + instrument shipped. **NO FIX. NO CAUSE NAMED.
OPEN-2 DOES NOT CLOSE.** (§NON-NEGOTIABLE 3.)

---

## §0 — PROVENANCE, AND THE BINARY IT IS ABOUT

**Artefact:** CI **34754906764**, `build-and-boot (shard 3)`, gate
**`smoke-nethammer`**, head **`be0b2ad`** on `dev/phase1-seyp3n`.

```
[schedcheck] next->rsp invalid tid=22 pid=22 rsp=0x0000000007D03A10
             base=0x0000000007D00000 rflags=0x0000000007D03A40
             r15=0xFFFFFFFF8001612D ret=0x0000000000000000 halting.
```

**The SECOND `[schedcheck]` fire in the project's history**, and the first to
carry DDR-1116's `r15=` and `ret=`. The capture also matched
`[blk] multi-inflight FAIL`, and the shard's own post-gate step printed
`kernel.bin: OK`.

**Binary identity read from the build job, not inferred (§INV.18):** job
`103717536008` published

```
f8574d7d6f0ba30ee6163c55177eb01227829c5ed3e63ad0dfc456bed0664021  kernel.bin
```

at **1,319,306 B** — byte-identical to DDR-1116's local build, so every address
below is resolved against the exact binary that produced the capture.
`be0b2ad` is docs-only over DDR-1116's `72e7b93`.

**The rest of the run was green:** 9 of 10 shards succeeded on that suite, and
the current tip `cd7ac5c` is all-green on **both** suites, 20 shard jobs.

---

## §1 — A CORRECTION TO DDR-1116 §2 BEFORE ANYTHING IS READ FROM IT

DDR-1116 §2 enumerated the return slot's legal set as two members and named the
first as **`0xffffffff800166ef`**, "after the unique `call context_switch` … at
`0xffffffff800166ea`". **That address is wrong for the binary DDR-1116 shipped.**
Re-measured against `f8574d7d6f0ba30e`:

```
ffffffff80016738: e8 73 9b fe ff   callq  0xffffffff800002b0 <context_switch>
ffffffff8001673d: e8 de f9 ff ff   callq  0xffffffff80016120 <finish_task_switch>
```

`llvm-objdump | grep -c 'call.*<context_switch>'` returns **1**, so the
enumeration is still exactly two members; only the numeral was wrong. The legal
set for this binary is:

| | address | origin |
|---|---|---|
| 1 | `0xffffffff8001673d` | after the kernel's only `call context_switch` |
| 2 | `0xffffffff800160d0` | `&thread_trampoline` (the seed at `sched.c:1182`) |

**The mechanism of the error is measured, not guessed.**
`0x80016738 − 0x800166ea = 0x4e = 78`, which is the size of DDR-1116's **own**
inserted code sitting ahead of that call inside `schedule_locked`. DDR-1116
recorded the address it measured **before** its own change and did not
re-measure after. `finish_task_switch` (`0x80016120`) and `thread_trampoline`
(`0x800160d0`) both lie *below* `schedule_locked` and did not move, which is
why the other numerals in that DDR are correct and only this one is not.

**WHY NOTHING CAUGHT IT, and this is the part worth carrying:** DDR-1116's two
mutants printed `ret=0xFFFFFFFF800160D0` (member 2) and `ret=0x0` (neither).
**Neither mutant ever printed member 1**, so the proof — which was a proof of
*discrimination*, and a sound one — never exercised the numeral that was wrong.
A two-member enumeration needs a witness for **each** member, or the unwitnessed
one is an assertion.

**This is §INV.18's exact failure occurring inside the DDR that invokes §INV.18**,
and DDR-1116 §9 had written the warning itself: *"a reader comparing a future
`ret=` against the numerals above rather than against its own binary reaches a
CONFIDENT WRONG ANSWER."* The reader it caught was its own successor.

**THE VERDICT BELOW IS UNAFFECTED:** `ret=0x0` matches neither the correct pair
nor the stale pair. What the error would have cost is a **future** capture
reading `ret=0x8001673d` — a *legal* value — being compared against `800166ef`,
found to mismatch, and routed to the wrong hypothesis.

---

## §2 — THE ARITHMETIC, BEFORE ANY READING OF IT

```
rsp  − base      = 0x3A10 = 14864          (8-aligned; 1520 B below ktop)
rsp + 64 − base  = 14928 <= 16384          clauses 2 and 3 PASSED
[rsp + 0x00]     = 0x07D03A40 = rsp + 0x30
[rsp + 0x08]     = 0xFFFFFFFF8001612D = finish_task_switch + 0xd
[rsp + 0x38]     = 0
```

`next->tid` / `next->pid` are printed (`sched.c:1719-1727`), so **tid 22 is the
INCOMING thread**. `pid != 0` makes it a ring-3 process, not a kernel thread.

**DDR-1115's fire had the same slot-0 relationship:** `0x07CB7A70 = 0x07CB7A40
+ 0x30`, also ~1500 B below its stack top. **Two of two.** That was recorded
there as "a stack address" and read as corruption; it is not.

---

## §3 — THE FINDING: THE TWO SLOTS ARE AN EXACT, INSTRUCTION-LEVEL MATCH FOR THE THREAD'S OWN RESUMPTION

Disassembled in this binary, not assumed:

```
ffffffff80016120 <finish_task_switch>:
  +0x00: 55              pushq %rbp
  +0x01: 48 89 e5        movq  %rsp, %rbp
  +0x04: 48 83 ec 20     subq  $0x20, %rsp
  +0x08: e8 43 37 03 00  callq <this_cpu>      <-- 5 bytes, next insn at +0x0d
ffffffff80049870 <this_cpu>:
  +0x00: 55              pushq %rbp
```

Let **S** be the value `context_switch` stored into `->rsp` (rsp after its seven
pushes). Resuming from S:

| step | rsp after | writes |
|---|---|---|
| `context_switch`: 7 pops | S+0x38 | |
| `ret` (pops `[S+0x38]`) | S+0x40 | → `0x8001673d` |
| `callq finish_task_switch` | S+0x38 | `[S+0x38] = 0x80016742` |
| fts `push rbp` | S+0x30 | `[S+0x30] =` caller rbp |
| fts `mov rsp,rbp` | — | **rbp = S+0x30** |
| fts `sub $0x20,rsp` | S+0x10 | |
| fts `callq this_cpu` | S+0x08 | **`[S+0x08] = finish_task_switch+0xd`** |
| `this_cpu` `push rbp` | S+0x00 | **`[S+0x00] = rbp = S+0x30`** |

**Both observed slots are produced at exactly those offsets, by that chain, with
no free parameters.** `[S+0]` is `this_cpu`'s saved frame pointer, which *is*
`S+0x30` by construction; `[S+8]` is the return address of the `call this_cpu`
that `finish_task_switch` issues at its own `+0x8`.

**So `next->rsp` is not a garbage pointer and the frame is not corrupt. It is a
CORRECT saved rsp whose frame HAS ALREADY BEEN CONSUMED** — the thread was
resumed from it, ran `finish_task_switch` → `this_cpu` on those very bytes, and
the scheduler is now attempting to resume it **a second time from the same
spent value**.

### §3.1 — `[S+0x38] = 0` is consistent, and I checked rather than waved
The chain writes `0x80016742` at `S+0x38`, and the capture reads `0`. That is
not a contradiction: `S+0x38` is the **shallowest** of the three slots, and every
subsequent call made by the thread from a depth above it rewrites it, whereas
`S+0x00` and `S+0x08` are rewritten only by calls that descend at least that far.
The shallow slot churns most; the deep pair retain the resumption's own litter.
**`ret=0` therefore carries "neither legal value" and nothing more** — the
routing datum did its job and its specific value is not evidence of anything.

### §3.2 — WHOSE LITTER IT IS
The stack belongs to tid 22 (`base` is tid 22's `kstack_base`, and `rsp` lies
inside that window), and **only tid 22 executes on tid 22's kernel stack.** A
different thread resumed from a frame at some S′ would leave the identical
pattern at *its* S′; for it to land at exactly this S requires a coincidence.
The reading that needs no coincidence is that the litter is tid 22's own.
**Stated as the simplest consistent reading, not as a proof.**

### §3.3 — THIS IS NEITHER OF DDR-1116'S TWO HYPOTHESES, AND THAT IS THE RESULT
DDR-1116 §3 split the space as **(A)** a real frame whose content was
overwritten, versus **(B)** a pointer that does not address a frame at all, and
said `ret` matching neither routes to (B) — where DDR-996's freed-while-queued
family and DDR-1096 §3's unlocked ring walk live.

**The field routed, and it routed OUT OF THE SPLIT.** This is a third case the
two-hypothesis frame did not contain: a frame that was **valid, used, and is
being used again**. The `ret` field is what made that visible — with only
`rflags=` (DDR-1115) the capture read as corruption, which is how DDR-1115 and
DDR-1099 both described it.

**The reading of OPEN-2 therefore inverts.** Everything from DDR-1099 onward has
been phrased as *something damaged the frame*. On this artefact nothing damaged
anything: the bytes are exactly what correct execution leaves. **The defect is
the SECOND SWITCH**, not the contents.

---

## §4 — WHAT IS NOT CLAIMED

**NO MECHANISM IS NAMED.** §3 says the thread was resumed twice from one saved
frame; it does **not** say why the scheduler selected it twice, and
§NON-NEGOTIABLE 3 forbids a fix on that. What was read, and found *correct*:

- **`rq_push` cannot double-enqueue.** It opens with an atomic exchange on
  `rq_on` and returns early if it was already set (`sched.c:185`), counting the
  skip in `g_ub_rqon_skip`. `rq_take`/`rq_unlink` clear `rq_on` on the pop
  (`:598`, `:488`). The "one queue, one popper" exclusion `sched.c:202` names
  holds on the push side.
- **The `on_cpu` handshake is correctly ordered.** `finish_task_switch` runs in
  the *resumed* thread's context and release-stores `pc->prev->on_cpu = -1`
  only after that thread's rsp is saved (`:634-642`); `switch_wait_offcpu_sched`
  acquires on it before any `->rsp` is read (`:704-724`).
- **The bounded wait's give-up path is not obviously the hole**: on timeout it
  re-pushes `next` and runs idle, and pushes and pops stay balanced.

**One occurrence of this reading; two `[schedcheck]` fires in total. NO RATE IS
CLAIMED and no campaign was run to manufacture one.** The DDR-1115 fire is
*consistent* with §3 on its slot-0 relationship, but it carries no `r15`/`ret`,
so it corroborates and does not independently establish.

**NOT ATTRIBUTED to `be0b2ad`, and NOT EXONERATED either (DDR-1042)** — that
commit is docs-only, which settles that the binary did not move, not that the
scheduler is innocent.

---

## §5 — THE INSTRUMENT, AND THE FIELD I REFUSED

The next occurrence should say whether §3's reading is right. Three tcb fields
were considered and **two of the obvious ones are constants here**:

- **`on_cpu` and `state` are NOT printed.** `sched.c:1563-1564` set them to
  `cpu` and `THREAD_RUNNING` **before** the check block at `:1670`, so each
  would print a fixed value dressed as live state — the **DDR-1093 `leaked=`
  trap** exactly, and the reason that DDR refused to print its own second field.
- **`rq_on` IS a variable.** The pop that selected this thread cleared it and
  nothing between that pop and the check touches it, so a healthy switch reads
  **0**; a **1** means the thread is simultaneously sitting in a runqueue while
  this CPU resumes it — which is precisely the state in which a second CPU can
  pop and resume it too.
- **`dispatches` / `switches_away` are an ARITHMETIC IDENTITY**, the shape
  DDR-1063 found actually works. `dispatches` is incremented under the claim at
  `:1565`; DDR-1118 adds `switches_away`, incremented at the single site that
  reaches `context_switch`. A thread is switched in, then away, then in — so at
  the check point **`disp == saves + 1` on every healthy switch**, and
  **`disp == saves + 2` is a thread switched in twice with no save between**:
  §3's reading as a number rather than as an argument about stack litter.

**PRINTED, NOT JUDGED — no fourth clause, and the refusal is load-bearing.**
DDR-1115's two clauses were admissible only because they hold **by the ISA**.
The identity holds by the code's habits: the increments are plain, and
`sched_exit` leaves by a path that does not pass the save site. A clause on it
could fire on a legitimate switch, and a false positive here **halts a CPU on
the hottest path in the kernel**. So **the set of frames that fire is UNCHANGED**
and only what the line *says* changes — DDR-1116's own refusal, applied again.

**COST:** one increment on a cacheline this path already writes (`prev->state`
above it, `prev->rsp` inside the call), plus three loads in the already-cold
`if (bad)` branch. DDR-1090's accepted precedent is two loads and a branch
inside `yield()`. `switches_away` gets an explicit initialiser in `sched_create`
(**NON-NEGOTIABLE 10**: `kmalloc` does not zero); `init_idle` memsets the tcb.

---

## §6 — PROOF IS DISCRIMINATION, NOT WIRING

The vacuity check first (**twentieth time caught in design text**): *"force a
fire and check `rq_on=` appears"* proves only that it prints. **M1-M3** therefore
share **one identical fire trigger** and differ from each other by
**exactly one line** (DDR-1042). **M4 (§7) necessarily uses a different trigger**,
because M1-M3's is misaligned-but-in-window and M4 must be out-of-window, so each new field is attributed on its own:

| | one-line delta from the trigger | predicts |
|---|---|---|
| **M1** | *(trigger only)* | `rq_on=0`, **`disp == saves + 1`** — the healthy identity, on a thread that has really run |
| **M2** | `next->switches_away--` | **`disp == saves + 2`** — a missed save; `rq_on` unchanged |
| **M3** | `rq_on = 1` | **`rq_on=1`** — the queued-while-resuming precondition; identity unchanged |

**M1 is the load-bearing control.** Without it, "the fields print numbers" and
"the identity holds on a healthy switch" are the same observation, and a field
that were always `saves + 2` would look like a detector while being a constant.

### §6.1 — MEASURED
Clean kernel `67922bdc46bd8fb0`; all three mutants built `-Werror` clean and
fired on `tid=14 pid=14`, a thread with **21 real dispatches**:

```
M1  ... rq_on=0 disp=21 saves=20 halting.      disp - saves = 1   IDENTITY HOLDS
M2  ... rq_on=0 disp=21 saves=19 halting.      disp - saves = 2   ANOMALOUS
M3  ... rq_on=1 disp=21 saves=20 halting.      rq_on flipped, identity intact
```

**Each lands on a DIFFERENT field and neither carries the other.** M1 is what
makes the other two mean anything: the identity is shown *holding* on a thread
that genuinely ran, so `saves` is neither a constant nor trivially skewed.

**M4 and the in-window regression, on the fixed tree:**

```
M1r ... rsp=0x07C6BA39 ... r15=0x0 ret=0x6A1234567890ABCD rq_on=0 disp=21 saves=20
        misaligned but IN-window -> still read, as intended.  LINELEN=197
M4  ... rsp=0x07C6C008 ... r15=? ret=? (rsp outside its own stack) rq_on=0 disp=21 saves=20
        base + STACK_SIZE + 8: 8-aligned, so clause 2 PASSES and only the window
        bound trips -- the exact combination the pre-§7 code dereferenced.
        The pointer is NAMED and NOT FOLLOWED.                LINELEN=191
```

M1r is the regression half: the guard must not stop the **in-window** reads,
which are the informative ones and are what both real fires produced.

**Line budget MEASURED, not assumed** (DDR-1116 §7): **LINELEN = 197** against
`KLINE_MAX = 256`, 59 bytes spare, on all three. An overflow emits
`[kline] TRUNC`, which is in `GLOBAL_FORBIDDEN` and would destroy the artefact
the line exists to carry.

### §6.2 — A MEASUREMENT DEFECT OF MY OWN, RECORDED NOT QUIETLY FIXED
The first run of all three reported **`MEASUREMENT-BROKEN: no capture`** and the
harness was what was wrong, not the kernel. `boot_test.sh`'s `serial_keep_fail`
**copies a failing run's capture to `${SERIAL_LOG}.fail-$$` and removes the
original** (DDR-988 §11) — and every mutant here fails by design, because the
check halts a CPU and the gate's sentinel never arrives. The harness polled
`$SERIAL_LOG`, which by then correctly no longer existed.

**The guard is what made this cheap:** the loop asserted the capture was
non-empty and printed `MEASUREMENT-BROKEN` rather than `0 fires`, so a broken
measurement could not read as a clean result (DDR-1092's rule; the same trap
DDR-1093 §7 hit and the `smoke-shell`/`SERIAL_LOG` trap recorded last session).
Had it grepped a missing file it would have counted zero and I would have
concluded the fields do not print.

**Carry the general form: a run that FAILS ON PURPOSE does not leave its capture
where a passing run would.**

---

## §7 — A DEFECT IN DDR-1116'S OWN SAFETY ARGUMENT, FOUND BY RUNNING THE MUTANTS

Every mutant printed `rflags=0x0` while still printing `r15=` and `ret=`. Tracing
why exposed a real defect in the code DDR-1116 shipped:

```c
int bad = (nrsp & 7u) || nrsp < nbase || nrsp + 64u > nbase + STACK_SIZE;
```

**`||` SHORT-CIRCUITS.** A misaligned `nrsp` sets `bad` at the first term and the
two bound comparisons are **never evaluated** — and DDR-1116's emit block then
dereferenced `nrsp + 0x08` and `nrsp + 0x38` **unconditionally**. So a pointer
that was *both* misaligned *and* outside the thread's stack was followed in the
report path.

**That is DDR-1079's defect** — the panic backtrace walker that faulted
mid-report and cost the machine a CPU — **reintroduced by the change that cited
DDR-1079 as its own reason for care.** DDR-1116 §4 claimed "clause 3 proved
`nrsp + 64 <= nbase + STACK_SIZE` BEFORE any dereference": true of the **RFLAGS**
read, which sits under `if (!bad)`, and **false of its own two reads**.

`rflags=0x0` is the tell and it is not a bug: `fl_slot` is only loaded when
`!bad`, so a clause-2 trip leaves it at its initialiser — which is exactly how
`bad` was reached without the bound.

**FIXED HERE:** `outside` is computed separately and unconditionally, and the two
reads are guarded on it. A misaligned-but-in-window pointer is still read (x86
permits unaligned loads and the whole 64-byte window is inside the thread's own
stack — the commoner and more informative case); an out-of-window pointer prints
**`r15=? ret=? (rsp outside its own stack)`** and is **not followed**. The
absence **names itself** (DDR-1049), so "refused to look" and "no `r15=` in the
capture" are not the same observation.

**NOTHING ELSE CHANGES:** `bad` is the same predicate, so **the set of frames
that fire is unchanged**; only what the line says in the out-of-window case, and
whether a distrusted pointer is followed.

**PROVEN BY M4**, which cannot be reached by M1-M3 (their trigger is
`rsp -= 7`, misaligned but in-window): `next->rsp = kstack_base + STACK_SIZE + 8`
is 8-aligned, so it passes clause 2 and trips only the window bound — the exact
combination the old code would have dereferenced.

### §7.1 — AND THE LINE I ADDED WOULD HAVE OVERFLOWED, WHICH THE MEASURED RUNS COULD NOT SHOW

The four mutants measured **197** and **191** bytes against `KLINE_MAX` 256 and
that looked comfortable. **It is the wrong number to be comfortable about.**
DDR-1116 §7's discipline is the **TYPE-BASED** worst case, and recomputing it for
the line *with* my three fields:

```
100  literal text
 90  five kline_x fields (5 x "0x" + 16 hex)
 20  tid + pid            (uint32_t -> 10 digits each)
 10  rq_on                (int, printed via (unsigned) -> 10 digits)
 40  disp + saves         (uint64_t -> 20 digits each)
---
260  against a usable 254  (kline_c truncates when n + 1 >= KLINE_MAX)
```

**That is an overflow, and an overflow emits `[kline] TRUNC` — which is in
`GLOBAL_FORBIDDEN` and would destroy the artefact this line exists to carry.**
The measured fires were small only because real tids and counters are small;
**this line fires precisely when the TCB is untrustworthy**, which is the worst
case to be relying on small values in.

**FIXED by making `rq_on` a single character (`0` / `1` / `?`), which is better
instrumentation as well as 9 bytes cheaper:** `rq_on` has exactly two legal
values, so anything else means the field itself is untrustworthy — and `?` says
that, where a raw garbage integer would read as a confident answer (DDR-1092's
rule, the same shape as the `kvalid=` guard that DDR added). Recomputed:
**251 against 254, margin 3**, and the arithmetic is now written at the site so
the next person adding a field recomputes rather than assumes.

**`KLINE_MAX` was deliberately NOT raised.** 24 call sites share that constant
and one of them is in `schedule_locked`, so widening it would grow the hottest
frame in the kernel to buy margin that a one-character field already buys.

**Three self-corrections in one change, all found by checking rather than by a
gate** (§1 the stale numeral, §7 the short-circuit, §7.1 the budget) — none of
which any measured run would have surfaced, because each concerns a case the
runs did not take.

### §7.2 — THE NEGATIVE, AND WHICH BINARY EACH RESULT WAS MEASURED ON

**The negative is the load-bearing half**: a false positive in this check halts a
CPU on the hottest path in the kernel, so "it fires when forced" is worthless
without "it is silent when it should be". On the **final shipping kernel
`6af029b001e6e6db`** (1,319,306 B), hash-pinned **before and after every gate**
(DDR-1060 §9):

```
smoke-shell          rc=0 pinned      smoke-rqstress       rc=0 pinned
smoke-smp            rc=0 pinned      smoke-blk-integrity  rc=0 pinned
smoke-smppreempt     rc=0 pinned
schedcheck lines in the shell capture: 0   (capture 50,253 B -- non-empty, so
                                            the measurement is valid, not absent)
hygiene: ALL EIGHT PASSED            GLOBAL_FORBIDDEN: 77
```

Four of the five run at `-smp 4`, including the heaviest create/exit churn gate.

**Binary provenance, stated rather than blurred.** Three kernels are involved and
each result belongs to one of them:

| kernel | what it is | what was measured on it |
|---|---|---|
| `67922bdc46bd8fb0` | fields added, **before** §7/§7.1 | **M1, M2, M3** |
| `10ff3fb1ebfb8f00` | + §7's window guard | **M1r, M4** |
| `6af029b001e6e6db` | + §7.1's single-char `rq_on` — **ships** | **the negative above** |

§7.1 changes only how `rq_on` is *formatted*, so M1-M3's discrimination claim —
which rests on `disp`/`saves` and on `rq_on` being 0 versus 1 — is unaffected by
it; **saying which binary each row came from is the point, not a caveat**
(§INV.18's discipline applied to my own results rather than to an address).

**A pipeline mistake of my own, for the third time in two sessions:** the gate
run's output was invisible while it ran because I piped it through `tail -30`,
which buffers until the pipeline ends. Same family as the `head`/SIGPIPE trap
that killed a build mid-run last session. **Do not put `head` or `tail` on a
pipeline you intend to watch.**

---

## §8 — NOT CLAIMED

- **NO FIX**, **NO mechanism named**, **OPEN-2 DOES NOT CLOSE**, no open issue
  moves (OPEN-1/2/12/13 untouched). §4 lists what was read and found correct.
- **NO RATE** — two fires ever, one of them carrying the new fields.
- **NO new clause**; the set of frames that fire is unchanged. **NO new gate**
  (179), **NO new sentinel** (`GLOBAL_FORBIDDEN` 77, `[schedcheck]` already
  entry 75), 79 probe ELFs.
- **NO defect found in the scheduler and none alleged** — `rq_push`,
  `rq_take`, the `on_cpu` handshake, `context_switch`, the frame layout and the
  seed are all correct for what they were built to do.
- **NOT EXONERATED IN ADVANCE (DDR-1042):** this adds an increment to the
  hottest path, so if the OPEN-2 signature moves, this commit is a candidate.
- **DDR-1115 and DDR-1116 are NOT withdrawn.** DDR-1115's clauses are correct
  and kept verbatim; DDR-1116's field is what produced this result and its
  discrimination proof stands. §1 corrects **one numeral** in DDR-1116 §2 and
  records why its own mutants could not have caught it; §7 fixes a real defect
  in its emit block. Both are the same shape — a claim that held for the reads
  DDR-1116 inherited and not for the two it added — and neither was reachable by
  the mutants it ran, which is the honest reason they survived rather than a
  failure of care.
- **NO out-of-window fire has ever been OBSERVED.** §7 fixes a path the two real
  fires did not take (both were in-window), so it removes a latent fault in the
  reporting path and is **not** a fix for OPEN-2 and not claimed as one.
- **DDR-1099 is NOT corrected.** Its R15 resolved to `finish_task_switch+0xd`
  against its own binary, and slot `+0x08` here holds the same *structural*
  value independently measured — corroboration across two binaries, which is
  what §INV.18's discipline is for.
