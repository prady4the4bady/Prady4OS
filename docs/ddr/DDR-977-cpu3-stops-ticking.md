# DDR-977 — B#3 root-caused: **an AP stops taking timer interrupts**; virtio-blk is the victim

> **§8 CORRECTS THIS DDR'S TITLE AND FRAMING.** §1-§7 say "CPU 3" throughout,
> from a uniform local sample. A later CI capture shows **CPU 1** frozen instead.
> The defect is *an AP* freezing early in boot; which one varies with which
> device happens to be routed at it. Read §8 before acting on §1-§7.

Status: ACCEPTED (root-cause localisation + instrument). **No fix in this DDR.**
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§INV.4).

**Answers:** DDR-976 §7's specified measurement.
**Supersedes:** DDR-976 §6's three candidate explanations — all three are refuted.

---

## 1. Result

The instrument DDR-976 §7 specified now prints, at every completion timeout, the
absolute tick counter of **every** CPU. Four `-smp 4` boots, 60 timeouts:

```text
[vblk] compl wait timeout unit=2 dest_cpu=3 dest_dticks=0 dest_abs=369
       bsp_abs=916  dest_present=1 ticks[916,872,870,369]  on_cpu=0 lba=200
[vblk] compl wait timeout unit=2 dest_cpu=3 dest_dticks=0 dest_abs=369
       bsp_abs=1416 dest_present=1 ticks[1416,1372,1370,369] on_cpu=0 lba=208
```

| boot | timeouts | `ticks[cpu0,cpu1,cpu2,cpu3]` at first → second timeout |
|---|---|---|
| 1 | 8 | `[916,872,870,`**`369`**`]` → `[1416,1372,1370,`**`369`**`]` |
| 2 | 20 | `[859,841,839,`**`336`**`]` → `[1359,1341,1339,`**`336`**`]` |
| 3 | 23 | `[818,800,800,`**`298`**`]` → `[1318,1300,1299,`**`298`**`]` |
| 4 | 9 | `[841,822,822,`**`320`**`]` → `[1341,1322,1321,`**`320`**`]` |

**CPUs 0, 1 and 2 advance by exactly +500 between consecutive timeouts — one
deadline's worth. CPU 3 does not advance at all, in any boot.**

`pc->ticks` is incremented in `sched_tick()`, which an AP reaches only from its
own LAPIC timer interrupt (`idt.c:293`). A frozen counter therefore means
**CPU 3 is not taking its timer interrupt.** It is not slow, not busy, not
descheduled: it is not being interrupted.

CPU 3 froze at 369 / 336 / 298 / 320 of its own ticks — a few hundred in, varying
per boot, while the other three were at ~800-900. So it ran normally for a while
and then stopped. `[smp] cpu 3 up id=3` / `locks OK` / `percpu OK` / `tss OK` all
print, and `[smp] cpus online=4/4`: it comes up fine and dies later.

## 2. The three DDR-976 §6 candidates are all refuted

1. **"the AP is halted and MSI-X is not waking it promptly"** — refuted as
   stated. This is not about MSI-X delivery: CPU 3's *own LAPIC timer* also stops
   arriving, and that has nothing to do with the block device's vector.
2. **"the AP is busy or briefly interrupt-masked"** — refuted. "Briefly" is
   wrong by orders of magnitude: the freeze is permanent for the rest of the boot
   (thousands of ticks), and a *busy* CPU would still take timer interrupts.
3. **"the programmed destination APIC ID is occasionally wrong / the message is
   lost"** — refuted. A wrong MSI-X destination cannot stop the target CPU's
   timer interrupt.

The real shape is a fourth thing none of them named: **one specific AP stops
taking interrupts entirely, part-way through boot.**

## 3. Why this presents as a block-device bug

`virtio_blk.c:342` routes each device's completion vector to an AP:
`dest_idx = 1 + (unit % (ncpu - 1))`. At `ncpu = 4`, unit 2 → **CPU 3**. So the
one device whose completions are pointed at the dead CPU is the one that times
out, and `unit=2 dest_cpu=3` is invariant across all 60 timeouts.

That is why DDR-878 found the block layer clean and was right to: **the block
layer is clean.** It is the only subsystem that *notices*, because it is the only
one that blocks on a 5-second deadline waiting for that CPU to do something.

**A discriminator I ran because the obvious inference was not safe.** "Only
unit 2 times out" is not by itself evidence that CPU 3 is the sick one — unit 2
is the SFS scratch disk the self-tests hammer, so it could simply have been the
only device with enough traffic to expose a fault all the APs shared. Dumping
*every* CPU's counter settles it: CPUs 1 and 2 keep ticking. The fault is CPU 3's
alone, not a shared AP fault seen through the busiest device.

**A corroboration I tried and had to withdraw.** The heartbeat's `rqcpus` field
looked like independent confirmation (4 → 3 early in one boot). It is not: a
0-timeout boot showed `rqcpus` at 3 and even 2, while a 23-timeout boot showed
mostly 4. That field tracks something else and is uncorrelated with the freeze.
Recorded so nobody re-derives it as evidence.

## 4. Consequences for the backlog

- **B#3's symptom is real and its subsystem attribution is wrong.** It is not a
  "virtio-blk completion stall"; it is an AP-liveness defect that the block layer
  reports. Renaming it matters: every previous attempt to fix it inside
  `virtio_blk.c` was looking in a subsystem that is behaving correctly.
- **It is `-smp`-specific because APs are** (DDR-976 §5: 0/10 at `-smp 1`,
  17/20 at `-smp 4`). At `-smp 1` there is no AP to wedge and the destination is
  the BSP.
- **The 5 s × N stall is a real cost even when gates pass** — up to 28 timeouts
  in one boot, each a failed I/O returning `-EIO` (DDR-976 §2).

## 5. What is still unknown — and why no fix ships here

*Why* CPU 3 stops taking interrupts. Candidates, none yet discriminated:

- it is spinning inside a `cli` region that never re-enables (a lock held across
  a path that does not restore flags);
- it took a fault whose handler does not return, leaving it wedged with
  interrupts masked;
- its LAPIC timer got masked or its LVT reprogrammed;
- it halted in a state where its LAPIC no longer delivers.

These need different fixes and the next instrument is a different one — e.g. have
each AP record its last-known RIP/lock state, or have the BSP NMI a CPU whose
`pc->ticks` has not advanced for N ticks and dump where it is. §NON-NEGOTIABLE 3
applies: there is now a named mechanism (CPU 3 not interrupted) but not yet a
named *cause*, and this project has twice paid for fixing the first without the
second.

**Why CPU 3 and not CPU 1 or 2** is itself the sharpest available lead: it is the
highest-indexed AP and the last brought up.

## 6. The instrument ships

`kernel/drivers/blk/virtio_blk.c` — the timeout message now carries `unit=`,
`dest_cpu=`, `dest_dticks=`, `dest_abs=`, `bsp_abs=`, `dest_present=`, `ticks[…]`,
`on_cpu=` and `lba=`.

It is **failure-path only**: the healthy path pays one `percpu_get` read and
prints nothing. That property is deliberate — DDR-947 records an instrument heavy
enough to move the failure rate it was measuring (2/12 → 9/14), and this one
cannot, because it executes only after a deadline has already expired.

The two gates that treat `[vblk] compl wait timeout` as a `FORBIDDEN_SENTINEL`
(`smoke-blk-timeout`, `smoke-rename-sfs`) match on substring, and the new line
still begins with that exact text, so both keep working unchanged.


---

## 7. Addendum — the window is tighter, and a confound I had to remove

### 7.1 A confound in "it only happens at `-smp 4`"

`-smp 2` (0/5 boots) and `-smp 3` (0/5) show no timeouts, against 21/24 at
`-smp 4`. The tempting conclusion — "the defect is specific to the 4-CPU
configuration" — **does not follow**, and the reason is in §3's own routing
formula: `dest_idx = 1 + (unit % (ncpu - 1))`. At `ncpu = 2` every device points
at CPU 1; at `ncpu = 3`, CPUs 1-2. **CPU 3 is only ever given a block vector when
`ncpu == 4`.** So at `-smp 2/3` a wedged CPU 3 would produce exactly the same
observation as a healthy one: nothing is waiting on it, so nothing times out.

The block-timeout path can only ever report on the CPU it happens to point at.
Every conclusion drawn from it inherits that blind spot.

### 7.2 The instrument that removes it

`kernel/idt.c` — the heartbeat now prints `cputicks[c0,c1,c2,c3]`, every CPU's own
LAPIC-timer tick count, on every heartbeat, at every `-smp`, independent of any
device routing. A counter that stops while the others climb is a wedged CPU, and
now it is visible whether or not anything is waiting on it.

Fixed-width numerics on a line already printed once per 500 ticks. The DDR-947
hazard was a *variable-length* `kputs` of a thread NAME inside the timer ISR
(which moved the failure rate it measured, 2/12 → 9/14); that is why `cur=`
remains gated behind `rd > 8` and this does not need to be.

### 7.3 What it shows — with a clean negative control

Three `-smp 4` boots, heartbeat at t=500 onward:

| boot | timeouts | `t=500` | `t=1000` | `t=1500` | `t=2000` |
|---|---|---|---|---|---|
| 1 | **0** | `[500,457,455,`**`451`**`]` | `[1000,955,954,`**`950`**`]` | `[1500,…,`**`1450`**`]` | `[2000,…,`**`1950`**`]` |
| 2 | 9 | `[500,483,482,`**`355`**`]` | `[1000,983,982,`**`355`**`]` | `[1500,…,`**`355`**`]` | `[2000,…,`**`355`**`]` |
| 3 | 17 | `[500,483,481,`**`303`**`]` | `[1000,982,981,`**`303`**`]` | `[1500,…,`**`303`**`]` | `[2000,…,`**`303`**`]` |

**Boot 1 is the control this investigation had been missing**: a `-smp 4` boot in
which CPU 3 tracks the others (+500 per heartbeat, a steady ~10% lag from its
later bringup) and in which there are **zero** timeouts. The correlation is exact
in both directions — CPU 3 alive ⇒ no timeouts; CPU 3 frozen ⇒ timeouts.

**And the window is much tighter than §1 said.** §1 put the freeze "a few hundred
ticks in". It is in fact **before the first heartbeat** — CPU 3 is already frozen
at t=500, at its own tick 303-355, i.e. inside the first ~3 seconds. Scaling by
the healthy lag, that is global tick ≈ 310-330, which in these boots falls just
after `[smp] user on AP OK` (t=287) and `[boot-load] PRISM.ELF` (t=282).

That is **not** enough to blame the user-on-AP probe: the healthy boot passes the
same milestone and survives. It bounds *when*, not *what*.

### 7.4 One-off anomaly, recorded and not diagnosed

One `-smp 3` boot (`build/gatelogs/live3.log`) produced 445 lines of normal boot
output, `[smp] rqstress OK`, and **zero heartbeats** — i.e. global ticking stopped
before t=500 while output continued. I suspected my own new instrument; two
`-smp 3` re-runs on the same kernel gave 23 heartbeats each, and `percpu_get` is
bounds-checked against `PERCPU_MAX = 16` with `g_percpu[PERCPU_MAX]`, so index 3
is in range. **The instrument is not the cause.** This is a separate, unreproduced
event that looks like the *BSP* side of the same class. Recorded here so it is not
lost, not merged into this defect on a resemblance.

---

## 8. CORRECTION — it is not CPU 3. **Any AP can freeze**, and this unifies OPEN-2

CI run `32600567390`, shard 3, head `848861b` (docs-only), gate **`smoke-resched`**:

```text
[hb] t=500  … cputicks[500,213,482,480]
[blk] multi-inflight FAIL done=0x0000000000000000 spawned=2/2
[vblk] compl wait timeout unit=0 dest_cpu=1 dest_dticks=0 dest_abs=213
       bsp_abs=788  ticks[788,213,770,768]  on_cpu=0 lba=1
[hb] t=1000 … cputicks[1000,213,982,980]
[vblk] compl wait timeout unit=0 dest_cpu=1 dest_dticks=0 dest_abs=213
       bsp_abs=1188 ticks[1188,213,1170,1168] on_cpu=0 lba=0
[smp] blk integrity FAIL reference-read
```

**CPU 1 is frozen at 213** while CPUs 0, 2 and 3 advance normally (+400 between
the two timeouts, +500 per heartbeat).

### 8.1 What this corrects

§1-§7 of this DDR say "CPU 3" throughout, because every capture I had — 4 boots,
60 timeouts, all local — showed `dest_cpu=3`. **That was a property of my
sample, not of the defect.** The title is wrong and the framing was too narrow:

> **The defect is that an AP freezes early in boot. *Which* AP varies.**

Why my sample was uniform: locally, only `unit=2` ever timed out, and
`dest_idx = 1 + (unit % (ncpu-1))` sends unit 2 to CPU 3. Unit 2 is the SFS
scratch disk the self-tests hammer, so it was the device most likely to catch a
frozen CPU — and it can only ever catch CPU 3. §7.1 already warned that the
block-timeout path "can only ever report on the CPU it happens to point at" and
that "every conclusion drawn from it inherits that blind spot". **This is that
blind spot, and I fell into it anyway** — I removed the confound for the
`-smp 2/3` question and did not re-apply the same reasoning to "which CPU".

Here it is `unit=0`, and `1 + (0 % 3) = 1` → CPU 1. Different device, different
AP, same freeze.

The freeze timing is consistent with §7.3: CPU 1 stopped at its own tick **213**,
already frozen by the first heartbeat (t=500), i.e. inside the first ~3 seconds —
the same window in which CPU 3 froze at 298-369.

### 8.2 What this unifies — OPEN-2 is downstream of B#3

`CLAUDE.md` lists **OPEN-2** as four intermittent `QEMU_SMP=4` gates:
`smoke-resched`, `smoke-blkmq-trace`, `smoke-msixap`, `smoke-crosswake`.

This capture is `smoke-resched` failing, and the failure chain is fully visible:

1. CPU 1 stops taking its timer interrupt (`dest_dticks=0`, `dest_abs` pinned).
2. Unit 0's completion MSI-X is routed to CPU 1, so its completions are never
   serviced → two `compl wait timeout`s → `submit()` returns `-EIO`.
3. `[blk] multi-inflight FAIL done=0x0 spawned=2/2` — both workers spawned, zero
   completions.
4. `[smp] blk integrity FAIL reference-read`.

So `smoke-resched` did not fail on a scheduler defect; it failed on block I/O
that could not complete because an AP was dead. **OPEN-2's block-touching gates
are B#3 seen through different sentinels.**

This is a *measured* unification, not a resemblance: the same capture contains
the frozen counter, the routing that points at it, the timeout, and the gate's
own failure line. Contrast DDR-880, which unified OPEN-10 with item 47 on a
shared signature and was corrected by DDR-884 for exactly that reason.

**Not claimed:** that *all four* OPEN-2 gates are this. `smoke-crosswake` and
`smoke-msixap` have not been captured with the instrument, and a gate that does
no block I/O could fail for its own reasons. The prediction is specific and
testable: a failing capture of those gates should contain a
`compl wait timeout … dest_dticks=0` with a frozen entry in `ticks[…]`.

### 8.3 Note on the removed instrument (DDR-980)

The `cputicks[…]` heartbeat was removed one commit before this capture landed,
and this capture used it. That does not reverse DDR-980: the **kept** `[vblk]`
instrument carries `ticks[…]` too, and it is what supplies the frozen-counter
evidence above at each timeout. What is lost is the *always-on* view — a boot in
which an AP freezes and **no device is routed to it** now shows nothing. For
OPEN-2's block-touching gates that gap does not bite. Re-add the heartbeat
version behind an opt-in flag if a non-block gate needs it.
