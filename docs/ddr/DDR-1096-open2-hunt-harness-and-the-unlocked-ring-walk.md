# DDR-1096 — OPEN-2 hunt harness: a third timing environment assessed, four of five
# approaches refused with measurements, and an unlocked ring walk in the timer ISR
# that is NOT claimed as the cause

**Status:** hunt infrastructure shipped (debug-flag only), campaign run, **no fix**.
**Scope:** §NON-NEGOTIABLE 3 governs throughout. No mechanism is named from a real
failing artefact in this DDR, so no fix is proposed and none is written.

---

## 0. What this DDR does and does not do

It does: assess the operator's five hunting approaches on their merits, refuse the
four that are unsound *for this specific race* with the measurement behind each
refusal, build the one that is sound, prove the instrument is wired, run a campaign
on a pinned kernel, and report the result.

It does **not**: name a mechanism, propose a fix, or move OPEN-2. A reading of the
source is not an artefact — DDR-1090 states that rule and it is not bent here.

---

## 1. The environment, measured before it was trusted

The operator's framing (PR #17, approach 5) is that a local WSL box is "a genuinely
different timing/load profile than both the isolated cloud sandbox and GitHub's
shared runners". **Two of the premises behind that do not hold, and both narrow what
a null result here rules out.**

**(a) This is not the 9p/DrvFS filesystem.** The checkout is on `/dev/sdd`, ext4
inside the WSL2 VM (`stat -f -c %T` → `ext2/ext3`), not a Windows passthrough. The
"WSL2's virtualized filesystem behaves very differently" concern does not apply.

**(b) The guest execution model is IDENTICAL to CI, not a third profile.**
`tools/qemu_runner/boot_test.sh` contains **zero** `-accel`/`-enable-kvm` flags
(measured: `grep -cE 'accel|enable-kvm'` → 0), so QEMU 8.2.2 defaults to **TCG**;
independently, the invoking user is not in the `kvm` group, so `/dev/kvm` is
unusable regardless. OPEN-2 is a race between emulated vCPUs, and TCG schedules
guest vCPUs with its own interleaving that is largely decoupled from host core
count. So this environment differs at the **host** level (4 cores, different host
scheduler, no CI tenancy) and is the **same emulator** at the level where the race
lives.

Corroborating that it is the same artefact: `make image` here produces
`kernel.bin` = **1,311,114 B**, byte-for-byte the size §CURRENT BUILD STATE records.

**Consequence, stated rather than discovered later: a null result in this
environment is weaker evidence than "a third timing profile came back clean".**
It is closer to "the same emulator, on a different host, came back clean".

---

## 2. The five approaches, assessed individually

### 2.1 — Dedicated isolated hunting job — SOUND, not built here
A separate workflow looping the suspect gates, off the release pipeline, is sound
and carries no risk to the 3-green criterion. It is **CI-side work and this session
is the local one**; building it would be guessing at runner behaviour from a box
that is not the runner, which is precisely the failure DDR-1045 paid for twice.
Recorded as buildable, not built.

### 2.2 — Concurrent stress (parallel instances) — **REFUSED**, two independent reasons
First, it is forbidden: §NON-NEGOTIABLE 12 says never run two QEMU instances
concurrently, and `boot_test.sh:154-164` **actively refuses to start** when another
is running. That alone settles it.

Second, and the reason the rule is right here: the race is between **vCPUs inside
one guest**. Running N guests does not raise intra-guest concurrency; under TCG it
oversubscribes the host and slows every guest roughly uniformly. Parallel guests
would also make any capture unattributable, since the gates share `build/`.

The *sound* residue of this idea — perturbing TCG vCPU thread interleaving via host
CPU contention — is real but is a different experiment from the one proposed, and
is not run here.

### 2.3 — Targeted delay injection — **SOUND, and the one built.** See §4.

### 2.4 — Wider time/hardware sampling — SOUND, not applicable locally
This is explicitly about GitHub Actions landing on varying invisible hardware. This
box is one machine; spreading runs across hours samples the same host. It has no
local form.

### 2.5 — Local WSL as a third profile — **PARTIALLY REFUSED**, see §1
Real at the host level, substantially weaker at the guest level than the framing
assumes, because the emulator is the same.

### 2.6 — And the plain repetition campaign is REFUSED on arithmetic, not on taste
Repeating the unmodified gate N times is the obvious move and it does not pay.
DDR-1082 already did this costing for `smoke-rqstress`: 0 failures in `n` gives a
95% upper bound of `1 - 0.05^(1/n)`, so **n=20 buys 13.9% single-binary** at
20 × 191 s ≈ **64 minutes** of foreground QEMU. DDR-1062 already bounds OPEN-2
**below 6.9% per suite** from 42 CI suites. So an unmodified local campaign costs an
hour to produce a bound **strictly weaker than evidence already on record**.

Delay injection is preferred because it changes the **probability**, rather than
spending the hour sampling it.

---

## 3. What reading the source found — NOT a mechanism, NOT a cause

Recorded because the next session should read its capture against it, exactly as
DDR-1080 recorded a narrowing without claiming a verdict.

OPEN-2's signature is an AP frozen inside its own timer ISR:
`isr_dispatch+0xfe7` under `sched_tick+0x2b1` ← `timer_tick+0x7f` ←
`isr_dispatch+0x2da` ← `isr_common.gs_kernel_in+0x8` (DDR-1006's producer).

`sched_tick` (`sched.c:1581-1602`) contains DDR-955's expiry sweep, which **walks
the whole all-threads ring** — `_t = _t->next` until `_t == current_thread` — under
`irq_save()` **and no cross-CPU lock**.

Measured in the tree, not reasoned:

| site | what it does to the ring | protection |
|---|---|---|
| `sched_tick` `:1588` | reads (full walk), **in the timer ISR, IF clear** | `irq_save()` only |
| `sched_snapshot` `:~1400` | reads (full walk) | `irq_save()` only |
| `sched_destroy` `:1434` | **writes** (`sched_ring_unlink`) | `irq_save()` only — its own comment says *"unlink under the lock"* |
| `reaper_thread` `:2026` | **writes** (`sched_ring_unlink`) | `irq_save()` only — its own comment says *"atomically under the lock"* |
| `find_zombie_child` | reads | **`g_sched_lock`** (DDR-1001's fix) |

**Two of those comments claim a lock that the code does not take.** `irq_save()`
masks IF locally; on SMP it excludes nothing on another CPU.

This is the shape DDR-1001 named and fixed **at `sys_wait4` only**. Its own comment
(`sched.c:1357-1373`) states the mechanism in general terms — *"A lock only excludes
participants who take it, so the reader could follow a `->next` mid-relink and never
come back… an unbounded loop… with IF clear, which nothing can preempt"* — and the
same reasoning was never applied to the ring reader that runs **inside the timer
ISR**, which is where OPEN-2's backtrace points.

**This is a hypothesis with a matching signature and it is NOT a finding.** A
matching mechanism is not an attribution (DDR-1056's own rule). And there is a real
design reason this site may have been left alone: `g_sched_lock` is the kernel's
hottest lock (DDR-1047 measured ~1.9M acquisitions), and taking it from the timer
ISR raises a deadlock question against every path that holds it. **Whatever the fix
is, it is not "add the lock", and it is not being designed here.**

---

## 4. The harness (debug-flag only)

`OPEN2_HUNT` (default **0**) and `OPEN2_RING_MAX` (default 4096), plumbed through
`KCFLAGS` beside the existing `KASAN`/`PIPE_TRACE`/`BSP_LIVENESS` flags. Under
`#if OPEN2_HUNT`, the `sched_tick` walk gains:

* **(a) a bounded `pause` per node** — widens the window in which another CPU can
  unlink and free the node this walk is standing on. It **only delays**: no writer
  is added and no product logic changes.
* **(b) a bound + poison test** — `KHEAP_DEBUG` memsets freed objects to
  `POISON_FREE` (0xDD), so a node freed underneath reads 0xDDDD… in every field.
  Tested **before** dereferencing `->next`, which is the load that would fault. On
  a hit it prints `[ringwalk] ESCAPED site=… n=… node=… state=… cur=…` and breaks.
  This is an **instrument**, not a fix: it reports and stops, so a wedged walk names
  itself instead of holding the CPU with IF clear for the rest of the boot.
* Printing is shot-limited to 8, on `ap_freeze_probe`'s own reasoning — an escaped
  walk recurs every tick on every CPU and the UART is ~87 µs/byte, so unbounded
  printing would stall the boot it is measuring (DDR-941).

**The shipped kernel is provably untouched:** with the flag at its default,
`kernel.bin` rebuilds **bit-identical** to the pre-change tree —
`0693e5b04685ad60`, 1,311,114 B, zero warnings at `-Werror`. Verified by rebuild,
not assumed.

### 4.1 The instrument is wired — proved, not asserted
Built with `OPEN2_RING_MAX=4`, a bound below any real ring size, so the escape arm
must fire. It did: **exactly 8** `[ringwalk] ESCAPED` lines (the shot limiter
working), each naming site, node, ring position and `current_thread`, and **the boot
continued healthily to t=8500 afterwards** — so the break-out is safe and the
instrument is not itself a wedge. Without this arm, "the harness found nothing" and
"the harness cannot print" would be the same observation (DDR-1041).

### 4.2 The pause was TUNED by measurement, and bigger is worse
The obvious move is to maximise the delay. It is wrong, and the reason is the point:

| `OPEN2_HUNT` | boot reached | `rqstress` (the unlink churn) |
|---|---|---|
| 32 | **t=17500** | **OK** |
| 128 | t=5500 | never completed |
| 512 | t=4500 | **never ran at all** |

Past ~32 the walk starves the boot, so the thread create/exit churn the race
*requires* never happens. **A wider window with no churn in it is a strictly worse
experiment.** 32 is the working point and the campaign uses it.

### 4.3 A continuous-churn harness was built and then REFUSED
The larger probability lever is unlink traffic, not delay: `rqstress_proof` supplies
one 24-thread burst per boot. A hunt-only thread creating workers continuously was
written, and **it is not shipped**, because it stopped being the system under test:

* first cut (4 workers per `yield()`): `[fs] create /KOUT.TXT failed`,
  `[exec] FAT32 placement of /EXECTEST.ELF failed`, `[sfs] mount failed`;
* paced to one worker per tick: mount recovered and `NEXUS KERNEL OK` returned, but
  `rqstress OK` was still absent and `ymask` read **0** against ~13M on a clean hunt
  boot — a different operating regime.

A reproduction obtained on a kernel that cannot mount its filesystem would not be
attributable to OPEN-2. Recorded with its measurement rather than quietly dropped,
and `kernel/main.c` is byte-identical to `HEAD`.

---

## 5. Two process defects found on the way

### 5.1 Changing a `-D` flag does NOT rebuild the kernel
`$(KERNEL_BIN)`'s prerequisites (`Makefile:433`) are **sources**; `KCFLAGS` is not
among them, and the per-object compiles are recipe lines of that one target, so
`rm -f build/sched.o` does not trigger them either. Measured: `make image
OPEN2_HUNT=32` after a `OPEN2_HUNT=1 OPEN2_RING_MAX=4` build **relinked the stale
object and produced the identical hash `88cf0c9dbfc233d2`** — i.e. it silently built
the wrong kernel. Caught only because the hash was checked.

This is §INV.10 (*"`make image` doesn't always rebuild `main.o`"*) generalised, and
it is sharper than that invariant states: the hazard is not one file, it is **any
flag-only change**. `touch` the source. **It is also exactly why DDR-1060 §9's
hash pin exists**, and this campaign checks the hash before *and* after every run.

### 5.2 §INV.3 refinement — the bracket form is not a complete guard
`pgrep -f "[q]emu-system-x86_64"` returned a live pid during pre-flight and the
process did not exist a second later. It was **this session's own `bash -c`**, whose
argv contained the literal string. The bracket trick avoids `pgrep` matching *its
own* argv; it does not avoid matching a **parent shell** whose command line mentions
the binary. A stray-QEMU check can therefore report a false positive from the very
command doing the checking. No stray QEMU existed at any point in this session.

---

## 6. Campaign

`tools/ci/open2_hunt_campaign.sh`, kernel pinned at **`4359e985efa922c7`**
(`OPEN2_HUNT=32`, `OPEN2_RING_MAX=4096`), `QEMU_SMP=4`, 180 s window, one QEMU at a
time. Per-run `SERIAL_LOG` (DDR-1023), hash verified before and after every run with
an **abort** rather than a warning (DDR-1060 §9), and every capture asserted to
contain `[hb]` before it is scanned — because a previous campaign here scanned make
output and its grep was vacuous.

Scanned for: `[ringwalk]`, `[apfreeze]`, `panic_stage=`, `NEXUS KERNEL PANIC`,
`gs FAIL`.

### 6.1 Result — 12/12 clean. **It did not reproduce.**

```
[campaign] kernel_pinned=4359e985efa922c7 runs=12 smp=4
[campaign] run=1..12  rc=0  [hb] t=17500..18000  clean
[campaign] DONE runs=12 signal_runs=0 kernel_pinned=4359e985efa922c7
```

Zero `[ringwalk]`, zero `[apfreeze]`, zero `panic_stage=`, zero
`NEXUS KERNEL PANIC`, zero `gs FAIL`. Every capture carried `[hb]` (so no scan was
vacuous) and every boot reached t≈17500-18000 with the full `rqstress` churn — i.e.
these are representative boots, not degraded ones. The pinned hash held before and
after all twelve runs; no stray QEMU at any point.

### 6.2 What this does and does not buy — the bound is the WEAKER half

**Stated first because it is the part most likely to be over-read:** 0 failures in
12 gives a 95% upper bound of `1 - 0.05^(1/12)` = **22.1% per boot**. DDR-1062
already bounds OPEN-2 **below 6.9% per suite** from 42 CI suites. **So this
campaign's bound is materially WORSE than evidence already on record, and nothing
should be concluded from it.** Twelve boots was never going to beat forty-two
suites, and the campaign was not run for the bound — §2.6 refused exactly that
trade.

What it *does* buy is narrower and is about the §3 hypothesis specifically:

> With `sched_tick`'s unlocked ring walk artificially slowed by 32 `pause` units
> **per node** — a large multiplier on the window in which another CPU can unlink
> and free the node that walk is standing on — **no walk escaped the ring and no
> poisoned node was ever observed**, across twelve boots that each completed the
> full 24-thread `rqstress` churn.

That is weak evidence *against* §3's race being easy to provoke by widening **that
particular** window. **It does not refute §3**, for a reason worth writing down:

**The instrument's coverage is asymmetric, and only one half is strong.**
* A node freed and left **poisoned** (0xDD) is caught directly and cheaply.
* A node freed and immediately **reused** by a new `sched_create` is NOT caught by
  the poison test — it reads as a valid TCB. Such a walk is caught only if it then
  fails to return to `current_thread` within `OPEN2_RING_MAX`. With a live ring of
  only ~15-30 threads, a walk thrown into a reused node may well re-enter the ring
  and terminate normally, leaving no trace at all.

So twelve clean runs say the **poisoned-node** path did not fire. They say
considerably less about the **reused-node** path, which is the one that produces an
unbounded loop rather than a fault. Recorded as measured-uncovered rather than
counted as coverage.

**A second limitation, from §1:** this is the same TCG emulator CI runs, on a
different host. A null here is not "a third timing profile came back clean".

---

## 7. NOT CLAIMED

* **No mechanism is named and no fix is proposed.** §3 is a source reading with a
  matching signature, which DDR-1056's own rule says is not an attribution.
* **OPEN-2 does not move.** No open issue changes state.
* **No defect is alleged in DDR-955, DDR-1001 or DDR-996.** DDR-1001's fix is
  correct for the site it covers; what §3 records is that the same reasoning was
  never applied to a second site.
* **The two misleading source comments in §3 are reported, not edited** — changing
  them is a product change on the paths OPEN-2 lives in, and this session is scoped
  to hunting.
* **No kernel change ships.** Default-flag `kernel.bin` is bit-identical
  (`0693e5b04685ad60`, 1,311,114 B), so the size/headroom pair and
  `ci-docstate-check` are unaffected; `GLOBAL_FORBIDDEN` 76; 179 gates unchanged;
  no new gate and none is proposed — a gate asserting "the freeze did not happen"
  is unfalsifiable at any N this project can afford (DDR-1082).
* **The continuous-churn harness is refused, not deferred** (§4.3), and the
  dedicated CI hunting job is recorded as buildable and not built (§2.1).
