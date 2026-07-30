# DDR-806 — the SMP proofs run behind the probe storm, and intermittently never run at all

**Status:** **diagnosis STANDS, proposed fix REFUTED and REVERTED.** The
reordering below was implemented and broke all three target gates locally. No
code from this DDR is in the tree. See "The fix was wrong" at the end — read
that before acting on anything above it.
**Date:** 2026-07-30
**Explains:** OPEN-1 (open since DDR-775, four hypotheses refuted), and the
`smoke-blkmq-trace` / `smoke-rqstress-liveness` CI reds of 2026-07-30.
**Relates to:** DDR-803 ("the durable fix for both is less boot work"), DDR-777
(discriminator), DDR-804 (wrongly suspected — see below).

## Evidence, before any hypothesis

Four consecutive commits — `9f1459a`, `6c375ea`, `c9a1537`, `d8c5c95` — carry a
**byte-identical kernel**; the last three are documentation-only. CI on the same
binary:

| run | commit | verdict | failing gate |
|---|---|---|---|
| 30504947387 | `9f1459a` | FAIL | `smoke-blkmq-trace` (`[blk] multi-inflight OK`) |
| 30505596073 | `6c375ea` | PASS | — |
| 30507516805 | `c9a1537` | FAIL | `smoke-rqstress-liveness` (`[smp] rqstress OK`) |
| 30509373522 | `d8c5c95` | PASS | — |

Same kernel, alternating verdicts, two *different* gates. This is intermittent
and cannot be a code regression — which retires DDR-804 as a suspect. It was
suspected on the strength of a local `smoke-shell` failure; CI shows
`smoke-shell` passing on the same commit, so that local red is a property of my
workstation, not of the tree (tracked separately).

## The mechanism

Both missing sentinels are emitted from `kmain`:

```
main.c:1311   smpuser_proof();      -> "[smp] user on AP OK"      <- OPEN-1
main.c:1312   blkmq_proof();        -> "[blk] multi-inflight OK"
main.c:1313   smp_blk_integrity();
main.c:1314   rqstress_proof();     -> "[smp] rqstress OK"
```

These sit roughly **180 lines after** the user-probe spawn block that begins at
`main.c:1134`. Every probe in that block is started with `sched_unblock` and then
competes with `kmain` for CPU.

The decisive detail is what is **absent**. Each proof prints either an `OK` or a
`FAIL` variant — `[smp] rqstress OK` / `[smp] rqstress FAIL`. In both failing
serials **neither string appears**. The proofs did not fail; they never executed.
`kmain` had not reached line 1311 when the gate's window closed, despite windows
of 180 s.

That distinguishes the two hypotheses that mattered without needing new
instrumentation: a proof that ran and failed would have printed `FAIL`, and the
`FORBIDDEN_SENTINEL` would have caught it. Absence of both variants can only mean
the line was never reached.

Why it is intermittent: `kmain` is an ordinary thread. Whether it reaches line
1311 in time depends on how much CPU the probe threads take first, which varies
with runner speed and `-smp 4` scheduling. Locally, where the host is fast, it
essentially always makes it. This is why OPEN-1 never reproduced on demand and
why four earlier hypotheses — all of which looked for a *defect* in the SMP path —
were refuted. There is no defect in the SMP path. The proofs are simply queued
behind work that does not need to precede them.

## Decision — move the proofs ahead of the probe storm

The proof block moves from after the user-probe spawns to immediately before
them, right after the DDR-734 allowlist self-test at `main.c:1133`.

Correctness of the new position, checked rather than assumed — at that point the
scheduler is running, APs are up, the block layer is live and SFS is mounted
(the probe block that follows depends on all of it, so it is all necessarily
established before line 1134). Each proof's own dependencies:

* `smpuser_proof` — APs + ring-3 spawn: both up.
* `blkmq_proof`, `smp_blk_integrity` — block layer: up, SFS is mounted from it.
* `rqstress_proof` — scheduler only: up.

Nothing in the proofs consumes state produced by the user probes; the dependency
runs the other way (probes need the FS the proofs merely exercise).

**This is not a timeout change.** No window is widened. The work is reordered so
a bounded, self-contained kernel self-test completes while `kmain` still owns the
CPU, instead of racing ~30 concurrent ring-3 threads for it.

## Why not raise the windows instead

Because it does not converge. The proofs run last, so their latency is a function
of total probe count, and the probe count grows every slice — DDR-800, DDR-801,
DDR-802 each added one. Any window chosen today is a window that has to be raised
again, and per DDR-803 these gates cannot early-exit, so every raise is paid in
full on every run. Reordering removes the dependency instead of repricing it.

## Verification, and its honest limit

The base failure rate is roughly 50% per run, so a single green CI run proves
nothing — two of the four runs above were green *with* the defect present.

* Locally: all affected gates must still pass, confirming the move breaks
  nothing. Locally they passed before too, so this is a regression check, **not**
  evidence the fix works.
* In CI: the claim is that `smoke-smpuser`, `smoke-blkmq-trace` and
  `smoke-rqstress-liveness` stop failing intermittently. Establishing that needs
  **several consecutive green runs**, not one. Until then this DDR's status is
  "mechanism named and addressed", not "proven fixed".

Recording that limit up front, because a single green run after this change will
look like proof and will not be.

## The fix was wrong — refuted on first run, and reverted

The reordering was implemented and built warning-free. All three target gates
then **failed locally**, on a kernel that passes them at the original position:

| gate | kernel `e8219c43ff84` (proofs hoisted) |
|---|---|
| `smoke-smpuser` | **FAIL** |
| `smoke-blkmq` | **FAIL** |
| `smoke-rqstress-liveness` | **FAIL** |
| `smoke`, `smoke-user`, `smoke-fs` | PASS |

The fix broke exactly the three gates it was written to repair, which is about as
clean a refutation as this kind of change can get.

### The reasoning error

I read line numbers as execution distance. The claim "the proofs sit ~180 lines
after the probe spawn block, so `kmain` takes a long time to reach them" is not
supported by the file: **`kmain` begins at `main.c:1822`**. Lines 1134 and 1311
are therefore inside a *helper* function that `kmain` calls, not inside `kmain`
at all. Distance-within-that-helper says nothing about how long `kmain` takes to
get anywhere, and `g_smp_have_aps` is assigned at `main.c:1915` — inside `kmain`,
*after* the helper's line numbers, which is a plain contradiction of the mental
model I had.

The relative order I relied on (probes spawn, then proofs run, same function) is
real. The explanation I built on top of it — that `kmain` is starved and never
arrives — is not established, and the failed reorder shows something between the
two positions is a **prerequisite** for the proofs rather than mere noise ahead
of them.

### What survives, and what does not

**Survives — the observation, which is still the strongest evidence available:**
each proof prints an `OK` or a `FAIL` variant, and in both CI serials *neither*
appears, so the proofs did not execute. And the four-commit byte-identical-kernel
table still proves the intermittency is not a code regression, which is what
retires DDR-804 as a suspect.

**Does not survive:** the causal story (CPU starvation) and the remedy
(hoisting). Both are withdrawn.

### What the next attempt must establish first

Do not move the proofs again until this is answered: **what, between
`main.c:1134` and `main.c:1311`, must run before the proofs work?** The failed
arm is the experiment that proves such a prerequisite exists — bisect the move
rather than guessing, by relocating the block to successively later points
between the two positions and finding where it starts passing. The first position
that passes names the dependency.

Only then is it worth asking why the proofs are sometimes not reached in CI.

### Rule this cost

Line proximity is not execution order. Before reasoning about "how long until
control reaches line N", confirm which function line N is actually in.

## Second correction — both of my explanations were wrong, and the evidence now
## narrows it properly

Two claims in the sections above are false. Correcting them with what the code
and the CI serials actually say.

### False claim 1 — "nothing in the proofs consumes state produced by the probes"

`smpuser_proof()` polls `while (!g_user_on_ap && g_ticks < dl)`. `g_user_on_ap`
is only set when a **user thread runs on an AP**, and the user threads are
created by the very probe block I hoisted the proofs above. The proofs depend on
the probes. That is why the hoist failed deterministically, and it means the
existing order is a **real dependency**, not incidental layout.

### False claim 2 — the line numbers, again

`kmain` is at `main.c:1805` (not 1822 — my earlier awk anchored on the wrong
line, and I published that figure before checking it). Lines 1134 and 1311 are
both inside **`fs_test_thread`** (`main.c:829`), a thread `sched_demo()` spawns
at `main.c:1580`. So the question was never "when does `kmain` arrive" — it is
"when does `fs_test_thread` arrive".

### The `g_smp_have_aps` race is also refuted

Tempting story: `fs_test_thread` reaches the proofs before `kmain` sets
`g_smp_have_aps`, so the shared guard returns silently — which is exactly
DDR-777's verdict (C). It is wrong twice over:

* ordering — `g_smp_have_aps` is assigned at `main.c:1898` and `sched_demo()` is
  called at `main.c:1924`, so the flag is set **before the thread is created**;
* measurement — in failing run 30507516805 the serial contains
  `[smp] cpus online=4/4`, `[smp] ap preempt OK` and `[smp] resched OK`. APs came
  up and the guard passed.

**DDR-777 verdict (C) is therefore refuted for these runs**, on evidence rather
than argument.

## Where this actually stands

Established, each from a specific artefact:

1. The proofs do not execute — neither the `OK` nor the `FAIL` variant appears,
   and the DDR-777 entry marker is absent too.
2. It is not the guard — APs are up in the failing run.
3. It is not a code regression — four commits, byte-identical kernel,
   FAIL/PASS/FAIL/PASS across two different gates.
4. The proofs cannot be hoisted — they require live user processes.

That leaves one candidate consistent with all four: **`fs_test_thread` does not
reach `main.c:1311` inside the window.** Between 1134 and 1311 sit ~30
`user_boot_from_sfs()` calls, and each is `vfs_create` + `vfs_write` +
`vfs_open` + read + `elf_load` — blocking SFS I/O over virtio-blk, contended
under `-smp 4` on a shared runner. This is DDR-803's "the durable fix is less
boot work", arriving as a real failure.

**Next measurement, and it must come before any fix:** print `g_ticks` at
`main.c:1134` and again at `main.c:1311`. That gives the elapsed cost of the
probe block directly. If a failing run never prints the second stamp, the
mechanism is confirmed and the fix is to shorten or reorder that block — for
instance moving the proofs to just after the *first* user probe (enough to make
`g_user_on_ap` reachable) rather than after all ~30. If both stamps print well
inside the window, this is refuted too and the search reopens.

Do not implement that reordering before the stamps are read. This DDR has
already produced two confident explanations that the next measurement destroyed.

## Revert verification — and a correction to the refutation table above

Re-running the three gates on the reverted kernel `f9d03ce220da` (byte-identical
to `d8c5c95`, whose CI run 30509373522 is green):

| gate | original position (`f9d03ce220da`) | hoisted (`e8219c43ff84`) |
|---|---|---|
| `smoke-smpuser` | **PASS** | FAIL |
| `smoke-blkmq` | **PASS** | FAIL |
| `smoke-rqstress-liveness` | **FAIL** | FAIL |

This corrects the refutation table earlier in this document, which implied the
hoist broke all three. It broke **two**. `smoke-rqstress-liveness` was *already*
failing locally at the original position, so its red in the hoisted arm cannot be
attributed to the hoist. The refutation of the hoist stands on `smoke-smpuser`
and `smoke-blkmq` alone — which is still decisive, but it is two gates, not
three, and the difference matters because it is the third gate that is now the
useful one.

### The genuinely new fact: OPEN-1 reproduces locally

`smoke-rqstress-liveness` fails **on this workstation**, on CI-green code. No
previous session achieved a local reproduction of the OPEN-1 family — every
earlier attempt reported "passes locally, fails in CI", which is what forced four
hypotheses to be argued from CI logs instead of measured.

That changes the economics of this investigation completely: the stamps can be
read against a locally failing run, iterated in minutes, instead of waiting on
~50% CI rolls. It also means "it only happens on slow CI runners" — an assumption
threaded through DDR-803 and the earlier DDR-806 reasoning — is **not required**
and may be wrong.

## THE SETTLING MEASUREMENT — the surviving candidate is refuted too

Two `g_ticks` stamps: **A** at the top of the probe block (`main.c:1134`), **B**
immediately before `smpuser_proof()` (`main.c:1311`). Pure instrumentation — two
prints, no lock, no wait, no behaviour change. Kernel `4923c1831f2a`, `-smp 4`,
`TIMEOUT_S=180`:

| run | A | B | Δ (probe block) | verdict |
|---|---|---|---|---|
| 1 | t=296 | t=695 | 399 ticks | PASS |
| 2 | t=296 | t=726 | 430 ticks | PASS |
| 3 | t=299 | t=723 | 424 ticks | PASS |

At 100 Hz that is **~4.2 s for the whole probe block**, with B reached at **~7 s
into a 180 s window** — roughly 96% of the window still unused.

**The candidate is wrong by more than an order of magnitude.** "~30
`user_boot_from_sfs()` calls consume the window" cannot be true: they cost 4
seconds. Every version of the "boot is too slow to reach the proofs" story —
including the one DDR-803 predicted and the one this DDR was built on — is now
dead. That is the third confident explanation this investigation has produced and
the third the next measurement has destroyed.

### Caveat, stated because it matters

This measurement used the **plain** kernel. `smoke-rqstress-liveness` rebuilds
with `BSP_LIVENESS=1`, so the artefact that actually fails is a different binary
with extra heartbeat output on the timer path. Per the specific-artefact rule the
measurement is being repeated against that exact build, aiming to catch a RED run
with the stamps present. The 96%-margin result is unlikely to be overturned by
heartbeat prints, but "unlikely" is not the standard this DDR has earned.

### Where the search reopens

With slowness eliminated, the remaining shape is: `fs_test_thread` reaches B
comfortably, and the proofs still sometimes produce neither `OK` nor `FAIL`. The
next hypotheses to separate — none of them yet supported, listed so the next
attempt does not start from zero:

1. **The proofs run but their output is lost.** DDR-790 already established that
   heavy trace output evicts markers from the 4 KiB log ring; `BSP_LIVENESS=1`
   adds exactly that. If the sentinel is *printed and then evicted*, every
   "never executed" conclusion in this document is an artefact of the ring, not
   of execution. The stamps + serial byte counts in the repeat run test this
   directly.
2. **`rqstress_proof()` blocks inside itself** after B — it waits on 24 worker
   threads, so a lost wake would hang it with no output either way.
3. Something after B but before the first `kputs` in the proof.

Hypothesis 1 is now the leading one, and it is nearly the opposite of everything
argued above: not "too slow to run" but "ran, and the evidence was overwritten".

## Hypothesis 1 (eviction) is refuted before it was ever tested

`kputc` (`kernel/console.c:63`) does **both**:

```c
void kputc(char c) {
    klog_putc(c);                             /* DDR-750: log ring (dmesg) */
    while ((inb(COM1 + 5) & 0x20) == 0) { }   /* wait for THRE */
    outb(COM1, (uint8_t)c);
}
```

The 4 KiB ring DDR-790 talks about feeds **`dmesg`**. The serial byte goes
straight out COM1, and `boot_test.sh` greps the **serial capture file**, which is
append-only. Nothing can evict a sentinel from it.

So "the proofs ran and their output was overwritten" is impossible, and the
`BSP_LIVENESS=1` serial-volume angle cannot work the way I proposed one section
above. I am retiring that hypothesis without spending the measurement on it —
the code answers it for free.

## A real S2 violation found on the way, independent of OPEN-1

That same line is an **unbounded busy-wait**:

```c
while ((inb(COM1 + 5) & 0x20) == 0) { }   /* no bound, no deadline */
```

and `kputs`/`kwrite` call it with **interrupts disabled** (`irq_save()` …
`irq_restore()`). If the UART's THRE bit does not set — a stalled or
back-pressured host-side pipe, a full QEMU serial buffer — the calling CPU spins
forever with IRQs off. That is a plain S2 violation ("every wait, loop, timeout
is bounded") sitting in the single most-used function in the kernel.

It is worth fixing on its own merit regardless of OPEN-1, and it needs its own
DDR — bounding it changes console behaviour under back-pressure, which every gate
depends on.

### And it is a credible OPEN-1 mechanism

It fits the evidence better than anything else so far:

* a stall inside `kputc` **after stamp B but before the proof's first `kputs`**
  yields exactly what the failing serials show — B present, then neither `OK` nor
  `FAIL`;
* other threads keep printing, because only the stalled CPU is spinning, which
  explains why the failing serials continue past the proofs with probe output;
* `BSP_LIVENESS=1` adds heartbeat traffic on the timer path, raising serial
  pressure — which is why the *liveness* variant is the gate that reproduces
  locally while plain `smoke-smpuser`/`smoke-blkmq` pass;
* it is intermittent by nature, because it depends on host-side drain timing.

**Not yet established.** The discriminator is already running: if a RED run shows
stamp **B present** with no `OK`/`FAIL`, the stall is after B and this becomes
the leading candidate. If B is **absent** in the red run, the stall is before B
and this is wrong too. Either way the answer comes from the artefact, not from
this argument.

## Confirmed against the exact failing artefact — candidate dead

Repeated with `BSP_LIVENESS=1` (kernel `32c84784cf9d`), the build
`smoke-rqstress-liveness` actually runs, same `TIMEOUT_S=180` / `QEMU_SMP=4` /
sentinels:

| run | A | B | Δ | heartbeats | serial | verdict |
|---|---|---|---|---|---|---|
| 1 | t=351 | t=931 | 580 | 35 | 14140 B | PASS |
| 2 | t=290 | t=693 | 403 | 36 | 14083 B | PASS |
| 3 | t=283 | t=676 | 393 | 35 | 14138 B | PASS |
| 4 | t=357 | t=973 | 616 | 32 | 14099 B | PASS |
| 5 | t=371 | t=1077 | 706 | 35 | 14164 B | PASS |

B is reached between **6.8 s and 10.8 s of a 180 s window**. There is no timing
pressure anywhere near the boundary — the margin is ~94%. The
"`fs_test_thread` runs out of window" family of explanations is closed.

Serial volume is also flat (~14.1 KB across all runs), so the
`BSP_LIVENESS=1` heartbeat traffic is not producing the pressure the retired
eviction hypothesis assumed either.

**What I did not get: a red run.** All 5 passed. The local failure rate is
therefore well under 1-in-5, so the earlier single local red was luckier than it
looked, and chasing one locally costs ~180 s per attempt (these gates declare
`FORBIDDEN_SENTINEL`, so per DDR-785 they cannot early-exit and every run burns
the full window).

## Decision: leave the stamps in and let CI catch it

Rather than spend ~30 minutes of local runs fishing for a red, the two stamps
stay in the tree permanently. They cost two `kputs` on a path that already
prints, and they convert every future intermittent red — in CI or locally — into
a decisive artefact:

* red with **B present** ⇒ `fs_test_thread` got there and the stall is *after* B,
  which points at the DDR-807 `kputc` spin;
* red with **B absent** ⇒ the stall is *before* B, and DDR-807 is not it either.

This is the cheapest way to make an intermittent defect testable: stop trying to
reproduce it, and make sure the next natural occurrence carries its own evidence.
The next red run answers the question without anyone having to be watching.

## Hypothesis column — current state

| # | hypothesis | status |
|---|---|---|
| 1 | Defect in the SMP path (4 variants, DDR-775…777) | refuted — proofs never emit either variant |
| 2 | `!g_smp_have_aps` guard / DDR-777 verdict (C) | **refuted** — failing serial has `cpus online=4/4`, `ap preempt OK`, `resched OK` |
| 3 | DDR-804 regression | refuted — 4 commits, byte-identical kernel, FAIL/PASS/FAIL/PASS |
| 4 | `kmain` starved, proofs hoistable | refuted — hoist broke `smoke-smpuser` + `smoke-blkmq`; proofs need live user procs |
| 5 | Probe block consumes the window | **refuted this slice** — B at 7–11 s of 180 s |
| 6 | Output evicted from a ring | refuted by inspection — serial is append-only to file; the ring feeds `dmesg` only |
| 7 | **`kputc` unbounded THRE spin, IRQs off (DDR-807)** | **open — awaiting a red run with stamps** |

Six refuted, one live. Every refutation came from an artefact or from the source,
none from argument.
