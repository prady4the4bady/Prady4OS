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
