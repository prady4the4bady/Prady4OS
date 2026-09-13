# DDR-1097 — The hunt detector was blind to the path it was hunting, and the isolated CI hunt job

**Date:** 2026-09-12
**Status:** SHIPPED — instrument widening + CI-side hunt infrastructure. **No fix.**
**Branch:** `dev/phase1-seyp3n`
**Follows:** DDR-1096 (the harness), operator instruction on PR #17 (five approaches)

---

## 0. What this DDR does and does not do

**Does:** closes the coverage limit DDR-1096 recorded against itself; reproduces
and then encodes a guard for the stale-build defect DDR-1096 §5.1 found; and
builds the operator's **approach #1** — a dedicated, isolated CI hunting
workflow that is not part of the release pipeline.

**Does NOT:** name a mechanism, fix anything, or move OPEN-2. **No artefact has
been captured.** §NON-NEGOTIABLE 3 holds and nothing here is a fix. The
hypothesis DDR-1096 §3 recorded (an unlocked all-threads ring walk in the timer
ISR) is **still a hypothesis**; this makes it *measurable per boot* instead of
observable only when it happens to wedge a CPU.

Default-flag `kernel.bin` is **bit-identical** — `0693e5b04685ad60`,
1,311,114 B — so the size/headroom pair and `ci-docstate-check` are unaffected.
`GLOBAL_FORBIDDEN` 76. **179 gates unchanged** — the hunt workflow registers no
`smoke-*` target and is not in the shard matrix.

---

## 1. The blind spot, and it is the path the harness exists to hunt

DDR-1096 §4 recorded its own limit in one sentence: *"the poison test catches a
node freed and left poisoned, but NOT one freed and immediately reused."*

That is not a corner. Read the detector as shipped:

```c
if (((uintptr_t)_t & 0xFFFFull) == 0xDDDDull   /* we followed a poisoned ->next */
    || _t->state == 0xDDDDDDDDu                /* the node is poisoned */
    || ++_hn > OPEN2_RING_MAX) {               /* the walk ran away */
```

All three arms describe a node that is **freed and still poisoned**, or a walk
that has **already run away**. Now follow the sequence the hypothesis is about:

1. the walk stands on node X and has not yet loaded `X->next`;
2. another CPU `sched_ring_unlink`s X and `kfree`s it — `cache_free` memsets
   `POISON_FREE`;
3. **`kmalloc` hands that same object straight back to `sched_create_state`**,
   which writes a fresh `tid`, `state`, `next` and `prev` and splices it into the
   ring at a different position;
4. the walk loads `X->next` — a *perfectly valid* ring pointer, to the wrong
   place.

At step 4 the address is a live heap object and `state` is a legal value, so
**arms 1 and 2 are false by construction**. Arm 3 is the only thing left, and it
is a backstop of 4096 iterations that fires long after the fact — and only if the
walk never happens to reach `current_thread` again. If the recycled node's new
ring position does lead back, the walk terminates normally and **nothing is
detected at all**.

So the detector saw the case that faults, and was blind to the case that loops —
which is the one OPEN-2's signature (a CPU frozen inside its own timer ISR, IF
clear, nothing able to preempt it) actually looks like.

---

## 2. The identity token is `tid`, and `pid` would have been useless

The fix is to sample the node's identity *before* the pause and re-read it
after: same address, different identity ⇒ the object was freed and re-allocated
underneath the walk.

The first choice was `pid`. **Measured, it does not work.** Every writer of
`->pid` on a ring node:

| site | writes |
|---|---|
| `sched.c:1102` | `t->pid = 0` — `sched_create`, because `kmalloc` does not zero |
| `sched.c:1213` | `t->pid = t->tid` — `sched_create_user` |
| `sched.c:1235` | `t->pid = t->tid` — `sched_create_user_clone` (fork) |

So **every kernel thread keeps `pid == 0`**, and the all-threads ring is mostly
kernel threads. A detector keyed on `pid` would be structurally silent for the
majority of nodes — the DDR-1059 shape, a control that reads stronger than it is.
Caught by enumerating the writers rather than by assuming.

`tid` is the right token and the reason is measurable, not stylistic:

* `grep -rn '\->tid *=' kernel/` returns **exactly one line** — `sched.c:1053`,
  `t->tid = next_tid++`, inside `sched_create_state`;
* `next_tid` (`sched.c:814`) is a `static uint32_t` that only ever increments.

So a TCB's `tid` is **assigned once at creation and unique for the life of the
boot**. Two different values read from one address across the pause therefore
mean one thing: the object at that address is not the object the walk was
standing on.

The poison case is covered twice over and that is deliberate: a node freed and
left poisoned reads `tid == 0xDDDDDDDD`, so this arm fires *earlier* than the
top-of-loop arm would, and the report prints **both** values so a reader can tell
`poisoned` from `reissued` without inferring it.

### 2.1 The window the pause already widens is the right one
Worth stating because it looks like it needs moving and does not. The loop is

```
check(_t) -> expiry(_t) -> _t = _t->next -> PAUSE -> back to check
```

so across the pause the walk **holds a pointer it has not yet dereferenced**.
That is exactly the "stand on a node, then load its `->next`" window the
hypothesis is about. The sample/compare goes around the existing pause; the pause
does not move, and product logic is untouched.

---

## 3. The arm that was designed and REFUSED

The obvious companion is to also latch `_t->next` and report if it changed across
the pause. **Refused, and not on taste:** the ring legitimately mutates under an
unlocked reader every time any thread is created or destroyed, and a relink that
leaves `_t` *in* the ring is harmless — the walk continues correctly. That arm
would fire on ordinary healthy churn, on every boot, and drown the one signal
that means something. It is the difference between "the ring changed" (constant,
uninteresting) and "the node I am standing on was destroyed and reissued"
(rare, and exactly the hypothesis).

---

## 4. What a `RECYCLED` line does and does not mean

Stated here so the next session does not over-read its own capture.

It means: **the hypothesised window is real and it opened on this boot.** A node
the timer-ISR walk was standing on was freed and re-allocated by another CPU
while that walk held it. Nothing in a healthy kernel produces that reading, and
the probe cannot manufacture it — the second `tid` is written by
`sched_create_state` on another CPU, which is the DDR-1066 "value you cannot
fake" shape.

It does **not** mean a CPU wedged, and it is **not** OPEN-2 reproducing. The
walk may still have found its way back to `current_thread`. So:

* a `RECYCLED` line is **evidence the precondition occurs**, with a rate;
* it is **not a mechanism** and **not an artefact of the freeze** (§NON-NEGOTIABLE 3).

### 4.1 A CORRECTION TO THIS DDR'S OWN DRAFT — `[ringwalk]` is ALREADY forbidden
This section first said the line *"must not go in `GLOBAL_FORBIDDEN`"*. **That
was written without checking the list, and it is wrong.** Running the forced
proof is what caught it: the boot returned `rc=1` with
`[scan] FAIL — forbidden pattern in capture: [ringwalk]`.

Measured: **`DDR-1001` put the bare prefix `[ringwalk]` in the list**
(`boot_test.sh:435`), for its own producer, and the entry matches on the prefix.
There are now **three** producers of that prefix:

| producer | which ring walk | in which build | means |
|---|---|---|---|
| `sys_wait.c:54` `[ringwalk] wait4 ring inconsistent pid=` | `sys_wait4`, **under `g_sched_lock`** | **SHIPPED** | DDR-1001: fatal by construction |
| `sched.c:1643` `[ringwalk] ESCAPED site=sched_tick` | timer ISR, unlocked | debug flag only | DDR-1096 |
| `sched.c:1700` `[ringwalk] RECYCLED site=sched_tick` | timer ISR, unlocked | debug flag only | this DDR |

Three consequences, and the list is **not changed** (`GLOBAL_FORBIDDEN` stays
**76** — nothing added, nothing removed):

1. **No regression in any gate or shipped image.** The two instrument producers
   are inside `#if OPEN2_HUNT`, which is 0 everywhere except a proof or hunt
   build, so in the 179 gates the entry means exactly what DDR-1001 says.
2. **A hunt fire reddens its own run for free**, which is the behaviour wanted
   and needs no new entry — the campaign's own regex is then a second, redundant
   detector rather than the only one.
3. **THE HAZARD, and it is the reason this is a section rather than a footnote:
   the matched PATTERN no longer identifies the SITE.** A scan report naming
   `[ringwalk]` could be DDR-1001's *product* detector or either debug
   instrument. The **lines** are self-identifying (`wait4` vs `site=sched_tick`),
   and DDR-1088's trailing-window fix means the report now prints them — so the
   disambiguation exists and costs one read. But reading the pattern instead of
   the line would attribute a debug instrument's fire to a shipped-kernel
   corruption detector, or the reverse. This is DDR-1019's rule ("resolve the RIP
   against its own binary before reading it as a known producer") arriving one
   level up, at a log pattern. **Splitting the entry into three was considered and
   refused:** it would add two patterns to a 76-entry append-only list for strings
   no gate build can emit, and §NON-NEGOTIABLE 6 records what appending to that
   list costs.

And that asymmetry is the whole value: the precondition is expected to occur far
more often than the wedge, so a hunt that can see it gets a number in hours
rather than waiting for a rare freeze. `total=` is printed on every report line
for the same reason DDR-1093 added `rqfree=` — a count with no denominator is not
a measurement (§NON-NEGOTIABLE 17). **Limit stated rather than hidden:** printing
is shot-limited to 8 (DDR-941's UART cost), so past the 8th line the total keeps
rising unobserved. The first line is the artefact; the total on the last printed
line is a lower bound and is labelled as one.

---

## 5. The stale-build trap, reproduced here, and a size check would be vacuous

DDR-1096 §5.1 found that `KCFLAGS` is not a prerequisite of `$(KERNEL_BIN)`.
**Re-measured independently in this environment, because the entire CI hunt rests
on it:**

| command | resulting `kernel.bin` |
|---|---|
| `make image` (default) | `0693e5b04685ad60`, 1,311,114 B |
| `make image OPEN2_HUNT=32` — **no touch** | `0693e5b04685ad60` — **IDENTICAL** |
| `touch kernel/proc/sched.c; make image OPEN2_HUNT=32` | `4359e985efa922c7`, 1,311,114 B |
| `touch …; make image` (back to default) | `0693e5b04685ad60` — bit-identical |

Row 2 is the trap in one line: a hunt campaign pointed at that binary runs a
kernel **with no harness compiled into it at all** and reports clean, forever,
with every hash check passing because the hash is stable — it is stably wrong.

`4359e985efa922c7` is **exactly** the kernel DDR-1096's WSL campaign pinned, so
the two environments build the hunt bit-identically; that is a provenance check,
not a claim about timing.

**AND THE CHEAP CHECK IS VACUOUS — measured before it was written** (the
fourteenth time this class has been caught in design text): both builds are
**1,311,114 B**, because the harness fits inside existing page padding. A
workflow asserting "the image changed" by *size* would pass on the stale binary.
**Only the hash discriminates**, so the hash is what the job asserts.

The guard is therefore a positive assertion in the hunt job itself: build
default → `A`; `touch` + build hunt → `B`; **fail the job if `A == B`**. It needs
no change to the shared build system, it survives someone later making `KCFLAGS`
a real prerequisite, and it fails loudly rather than silently if the `touch` is
ever dropped.

**RECORDED, NOT ACTED ON** (operator's standing report-don't-act instruction):
the general remedy is a stamp file holding the flag values, regenerated per
`make` run and rewritten only on change, listed as a prerequisite of
`$(KERNEL_BIN)` — that would fix §INV.10's generalisation for *every* flag
(`KASAN`, `PIPE_TRACE`, `BSP_LIVENESS`, `OPEN2_*`) instead of one job checking
one of them. It is a change to the build system every gate depends on, days from
a held release, and it is not made here.

---

## 6. The workflow — approach #1, and what it is NOT

`.github/workflows/open2-hunt.yml`, `open2-hunt`.

* **Isolated by construction.** `workflow_dispatch` + `schedule` only —
  **no `push`, no `pull_request`**. It cannot appear in the 3-green promotion
  criterion (§NON-NEGOTIABLE 1), cannot redden a release suite, and registers no
  `smoke-*` target, so `ci-shard-check` is untouched at 179 gates.
* **A red hunt job is the deliverable, not a regression.** The job fails when a
  signal is found, so the run goes red, the notification fires, and the capture
  is uploaded. That is the opposite polarity from every other workflow here and
  it is stated so nobody "fixes" it.
* **The `schedule:` cron is approach #4**, obtained for free. DDR-1096 §2.4
  judged wider time/hardware sampling sound but with no local form; a weekly
  dispatch spreads runs across whatever hardware GitHub happens to allocate,
  which is the only handle this project has on that variable.
* **The matrix is NOT approach #2.** DDR-1096 §2.2 refused concurrent parallel
  instances twice over, and that refusal **stands**: the race is between vCPUs
  *inside one guest*, so N guests on one host raise no intra-guest concurrency
  under TCG, and §NON-NEGOTIABLE 12 forbids two QEMUs per machine anyway. The
  matrix here puts **one QEMU on each of N separate runners** — it buys
  throughput and hardware diversity and explicitly **not** concurrency. Reading
  it as a revival of the refused approach would be wrong, which is why the
  workflow says so in its own comments.

### 6.1 IT IS BUILT AND IT IS NOT YET RUNNABLE — measured, and the remedy is one file

Stated plainly because "approach #1 is done" would be an over-claim.

Dispatching it returns **`404 Not Found`**
(`POST /actions/workflows/open2-hunt.yml/dispatches`, `ref=dev/phase1-seyp3n`),
and `list_workflows` returns **2 workflows** — `ci.yml` and Dependabot — with
`open2-hunt` **absent**. That is not a defect in the file and not registration
lag: **a `workflow_dispatch` workflow must exist on the repository's DEFAULT
branch before it can be dispatched on any ref**, and this repo's default branch
is **`dev/phase1`** (measured: `git remote show origin` → `HEAD branch:
dev/phase1`; `ci.yml`'s own `html_url` is rooted there). The file is currently
only on `dev/phase1-seyp3n`, the head of PR #17.

**The remedy is to land `.github/workflows/open2-hunt.yml` on `dev/phase1`**, and
the isolation design is exactly what makes that cheap: the workflow has **no
`push` and no `pull_request` trigger**, so putting it on the default branch adds
**zero** CI load, cannot run on any commit, and cannot enter the 3-green
criterion. It sits inert until someone dispatches it. Either PR #17 merging or a
one-file PR would do it.

**NOT DONE HERE, and the reason is a standing instruction, not a judgement:**
this session develops on `dev/phase1-seyp3n` and must never push to another
branch without explicit permission. So the constraint is recorded and the
decision is the operator's.

Each lane: install the shard job's exact package set, `make musl` + `make lwip`,
build default (hash `A`), `touch` + build hunt (hash `B`), **assert `A != B`**,
build the FAT/SFS/ext4 data disks so the guest boots the same topology the local
campaign did, then run `tools/ci/open2_hunt_campaign.sh`, which already pins the
hash before and after every run (DDR-1060 §9) and refuses a capture with no boot
output (DDR-1023). Captures upload on success and failure alike.

---

## 7. Proof

**The new arm is wired — forced, not asserted** (DDR-1096 §4.1's own standard,
and the DDR-1030/DDR-1047 standard for an instrument whose triggering condition
cannot be manufactured in product): built with the comparison inverted so it
fires on every node.

### 7.1 The forced run, measured

`touch kernel/proc/sched.c && make image OPEN2_HUNT=32 OPEN2_FORCE_RECYCLE=1`
→ kernel `9ff5715403f73284`, warning-clean at `-Werror`; one boot at
`QEMU_SMP=4`, 180 s window.

| checked | result |
|---|---|
| `[ringwalk] RECYCLED` lines | **exactly 8** — the shot limiter works |
| `total=` | increments 1, 2, 3 … — the denominator is wired, not decorative |
| `tid0=` / `tid1=` | both print on every line |
| boot after the arm fired | **`[hb] t=17500`** — it kept running |
| `NEXUS KERNEL OK` | present |
| `rqstress OK` | present — the unlink churn still ran |
| harness rc | **1**, `[scan] FAIL — forbidden pattern: [ringwalk]` |

The last row is the **expected** outcome, not a defect: §4.1's entry means a
fire reddens its own run. The first six are what the forced build is for — the
branch is reachable, prints a complete line, breaks out safely, and **does not
itself wedge the boot**, which is the property that matters for an instrument
sitting in the timer ISR with IF clear. Without this, "the hunt found nothing"
and "the hunt cannot print" would be the same observation (DDR-1041).

**One honest limit of the forced build, stated because the numbers above look
better than they are:** with the comparison inverted the arm fires on node 1 of
every tick, so DDR-955's expiry sweep breaks out immediately and effectively does
not run. The boot surviving that is evidence the *break-out* is safe; it is not
evidence about a real fire, which happens once and then stops. The forced build
is a proof of wiring and nothing else, which is why it is behind its own flag and
default 0.

There is **no mutant for the discriminating half** and that is a stated
limitation, not an oversight: what makes a mismatch mean *recycle* is that `tid`
has exactly one writer and `next_tid` only increments (§2), which is established
by reading the tree, and no mutation can establish it. What a mutation can prove
— that the branch is reachable, prints, breaks safely and does not itself wedge
the boot — is what §7.1 proves.

---

### 7.2 A SECOND FALSE-CLEAN PATH, found by running the loop once — on the RUN side

The operator's instruction named a false-clean on the **build** side (§5). Three
boots of the real hunt kernel `e88739745b21238a` found another on the **run**
side, and the campaign scored it as evidence:

| run | heartbeat reached | `[smp] rqstress OK` | `ymask` | campaign said |
|---|---|---|---|---|
| 1 | t=8000 | **yes** | 141,354 | clean |
| 2 | t=7500 | **yes** | 106,596 | clean |
| 3 | t=9500 | **NO** | **1,085,721** | clean |

Run 3 was not hung — heartbeats arrived to t=9500 and it used its whole window.
It was **starved**: `ymask` an order of magnitude higher says the boot spent that
window yield-spinning, and it never reached `rqstress_proof`, whose 24-thread
create/exit burst is where a gate boot's unlink churn comes from. **A boot with a
wide window and no churn in it cannot fire, so scoring it `clean` counts a run
that tested nothing.** Over a long CI hunt that silently deflates the rate the
whole exercise exists to measure — the DDR-1023 vacuous-capture class, one level
in: that guard asks whether the capture has boot output, and this capture had
plenty.

It is also a **measured refinement of DDR-1096 §4.2**. That table recorded
32 → `t=17500`, `rqstress OK`, and 128/512 → starved, and concluded 32 is the
working point. It is the *right* working point and it is **not reliably below the
starvation threshold** — here it starved 1 boot in 3, on a host that builds the
identical binary. One observation of a boundary is not the boundary.

Fixed where it belongs, in the campaign rather than in the tuning: each run is
classified `churn` / `NO-CHURN`, and the summary carries `churn_runs=` beside
`signal_runs=` so any later bound has a denominator that means something
(§NON-NEGOTIABLE 17). The classifier was checked against the three captures
already on disk — no extra QEMU — and agrees with the independent measurement.

The CI job fails a lane on `churn_runs=0` and **not** on a merely low count: some
starved runs are ordinary noise, whereas a lane where none had churn is reporting
on an experiment that did not happen. **`OPEN2_HUNT` is deliberately NOT retuned
here** — that needs its own measurement across hosts, and lowering it on one
host's three boots would be the mistake this section is about.

---

## 8. NOT CLAIMED

* **NO fix, NO mechanism, NO attribution.** OPEN-2 does not move; OPEN-1/12/13
  are untouched. DDR-1096 §3 remains a hypothesis with a matching signature, and
  a matching mechanism is not an attribution (DDR-1056).
* **NO artefact was captured.** Nothing fired in the local verification runs; a
  `RECYCLED` line has never been observed. The hunt has not found anything yet —
  it has been made able to.
* **NO rate is claimed.** Not from DDR-1096's 12 boots and not from anything here.
* **NO product change.** Everything new is under `#if OPEN2_HUNT`, default 0;
  `kernel.bin` at the default flag is bit-identical, verified by rebuild.
* **NO gate added** (179 unchanged), **NO `GLOBAL_FORBIDDEN` change** (76), and
  the hunt workflow is deliberately outside the release pipeline.
* **The build-system remedy is recorded and NOT built** (§5).
* **`OPEN2_RING_MAX` is NOT retuned** — 4096 stays the backstop; the new arm is
  the primary detector and changing the bound is a separate knob with its own
  measurement.
