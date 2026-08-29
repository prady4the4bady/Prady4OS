# DDR-990 — the two-CPU `connect`/`close` hammer: design

**Status:** IMPLEMENTED and mutation-checked in both directions — see §10-§12.
(This line read "DESIGN. Not implemented." until DDR-993; the §8-§12 results were
appended without updating it, so the document contradicted itself at the top.)
**Relates:** DDR-987 (the lwIP core lock), DDR-988 (deferred work), OPEN-1.

---

## 1. The measurement that prompted this, and what it is worth

`smoke-surfdestroy`, 20 runs, kernel `e3919140872fd2ea` (the PR #13 merged tip,
verified byte-identical to `dev/phase1` `2cd7db9` apart from a docs file):

```text
20/20 pass, 0 fail
```

Prior baseline: **19/20** on `d31b4023b0f74d06` @ `46ece3f` (DDR-985).

**This is not proof, and the improvement is not statistically meaningful.**
At the measured base rate of ~1 failure in 20 runs (p ≈ 0.05):

```text
P(0 failures in 20 | defect unchanged) = 0.95^20 ≈ 0.358
```

So a clean 20-run sweep happens **roughly one time in three even if the defect
is entirely untouched**. The campaign has ~64% power. And 19/20 → 20/20 is one
fewer failure in twenty trials, comfortably inside noise for p ≈ 0.05.

To reach 95% confidence of catching a 1/20 defect by sampling alone:

```text
0.95^n < 0.05  =>  n > ln(0.05)/ln(0.95) ≈ 58.4   =>  ~59 clean runs
```

Sixty boots is ~2 hours of QEMU to reach a conclusion that is still only
"probably absent". That is the wrong instrument.

## 2. Why sampling is the wrong instrument here

`smoke-surfdestroy` does not target the DDR-987 defect. It exercises surface
churn and trips the race only incidentally, which is exactly why its rate is
~1/20 rather than ~1/1. Sampling an incidental trigger is expensive and weak.

The defect has a **named mechanism** (DDR-987): cpu A walks `pcb->unsent` inside
`tcp_output`, reached from `sys_sock_connect`, while cpu B frees that seg via
`tcp_abort` from `psock_close` (or via `tcp_tmr`/`tcp_input` from the timer
ISR). A probe that drives that shape directly should reproduce in seconds, not
in one boot out of twenty.

That converts the question from "did we fail to see it?" to "does it still
happen when we try to make it happen?", which is answerable.

## 3. Design

`user/nethammer.c`, a freestanding ring-3 probe (raw syscalls, no writable
globals per DDR-826, stack buffers inside the 8 eager pages of ADR-038).

**Spawned TWICE**, as two processes, followed by `smp_resched_all()` so the idle
APs pick them up (the DDR-966 lesson: workers spawned without it sit on halted
APs while the BSP burns the deadline). On `-smp 4` the two land on different
CPUs, which is the whole point — a single-CPU hammer proves nothing about a
cross-CPU race.

Each instance loops `N` times:

1. `SYS_SOCK_CONNECT(0x7F000001, 8007)` — loopback to the in-kernel TCP echo
   server that `net_init` already binds (`lwip_port.c:374`). Loopback completes
   without hardware and drives the full `tcp_connect` → `tcp_output` → seg-alloc
   path.
2. `SYS_SOCK_CLOSE(fd)` immediately, whatever came back — including on failure,
   so a handle is never leaked and the `tcp_abort` teardown path is hit on the
   same cadence as the connect path.

The two instances therefore interleave *allocate* and *free* of `tcp_seg` /
`pcb` on two CPUs with no phase relationship — the DDR-987 shape, on purpose.

**Sentinel:** `PRADYOS_NETHAMMER_OK id=<0|1> iters=<n> conn_ok=<n> conn_err=<n>`
— printed only after all `N` iterations complete. Per R17 the counts are the
denominator: `conn_ok` alone would hide a run where every connect was refused
and the probe therefore exercised nothing.

**Pass criterion:** both instances print their sentinel and the kernel survives.
Panics are already caught — `GLOBAL_FORBIDDEN` fails any gate on a panic banner,
and DDR-988 §11 now preserves the serial capture of a failing run.

## 4. The falsifiability requirement — the part that decides whether this is worth anything

**A hammer that passes on both the fixed and the unfixed kernel proves nothing.**
That is the DDR-988 §9 failure mode exactly: `smoke-net-fuzz` was green while
613 of its 768 frames never reached lwIP, because its pass criterion was
survival and dropping a frame survives reliably.

So this probe is **not** to be recorded as evidence until it has been
mutation-checked in both directions:

| kernel | required outcome |
|---|---|
| `g_net_lock` **reverted** (DDR-987 backed out) | **MUST panic or corrupt**, within a bounded `N` |
| current (`g_net_lock` present) | must complete `N` iterations clean |

If the reverted kernel survives the hammer, **the probe is too weak and must be
strengthened before it is trusted** — raise `N`, add a third instance, or
shorten the connect/close spacing. Recording a green hammer as proof without
that check would be worse than not having the probe, because it would look like
the closure OPEN-1 has been waiting for.

`N` is to be chosen from the *reverted-kernel* reproduction: pick `N` at ~10x
the iteration count at which the unfixed kernel first faults, so the fixed
kernel's clean run has real power behind it.

## 5. Credential choice — and the audit-churn trap

`sys_sock_connect` (`sys_socket.c`) gates in this order: privacy mode → CAP_NET
→ egress allowlist, with `is_sovereign` bypassing the last two.

- **Sovereign** is the easy path, and is **wrong here.** Every sovereign connect
  writes an `AR_SOVEREIGN_BYPASS` audit record (DDR-800). At hammer rates that
  is thousands of records, which churns the audit ring and perturbs the very
  timing being measured. An instrument must not dominate what it measures — the
  DDR-947 lesson, where printing a thread name every heartbeat moved the failure
  rate from 2/12 to 9/14.
- **Therefore `is_net = 1`, not sovereign**, using the
  `elf_load` → set flag → `sched_unblock` pattern (`main.c` ~1526, since
  `user_boot_from_sfs` cannot grant `is_net`).

**Open implementation point:** `netallow_check(127.0.0.1, 8007)` must pass, or
the probe gets an audited `-EPERM` and hammers nothing — a vacuous probe with a
green sentinel. Resolve at implementation by either confirming loopback is
already on the boot allowlist, or adding that one row. **Verify by asserting
`conn_err == 0`**, which is precisely why §3 prints both counters.

## 6. Gating

Opt-in via `probe_enabled("nethammer")` (DDR-804), registered as its own gate
`smoke-nethammer` at `QEMU_SMP=4`. Not spawned in every boot: a probe that
hammers the network stack in all 149 gates would add load and jitter to every
one of them for no benefit — the same reasoning `main.c` already records for the
privacy-netfilter probe.

## 7. What this will and will not establish

**Will**, once mutation-checked: that the specific cross-CPU
allocate/free race DDR-987 named no longer reproduces under direct pressure.
That is the positive evidence DDR-987 §5 says is missing and that no amount of
`smoke-surfdestroy` sampling can supply.

**Will not:** close OPEN-1 by itself. OPEN-1's artefact is a `#PF`; DDR-987's is
a `#GP` with `RAX=0xDDDDDDDDDDDDDDDD`. That they are the same defect has never
been established (DDR-985 refuted its own Claim A). A green hammer plus a clean
20x `smoke-surfdestroy` together are a strong case, and still a case — not a
proof. Say so when recording it.

---

# IMPLEMENTED — the mutation check, and what it establishes

§4 said this probe is only evidence once mutation-checked in both directions.
It has been. This section is the result.

## 8. The mutation

**The mutation removes exactly one property and nothing else.** Local interrupts
are still masked precisely as the pre-DDR-987 code masked them, `g_net_rxq_lock`
is untouched, and `net_unlock()` still drains. The only thing withdrawn is *two
CPUs cannot be inside lwIP at once* — the property DDR-987 added and the one
this probe claims to test. 13 acquires, 1 release, 1 trylock and 1 bare unlock
were neutered; `-Werror` then flagged `g_net_lock` as unused, which is itself
confirmation that no use survived.

Both kernels were built and their hashes compared, so neither run could have
been a stale binary:

| kernel | hash |
|---|---|
| fixed | `abb6b8f582727b6e` |
| mutant (mutual exclusion removed) | `ad3686405d4912d0` |

## 9. Result

| kernel | outcome |
|---|---|
| **fixed** | both instances complete. `conn_ok=20000 conn_err=0` **each** — 40,000 connect/close pairs across two CPUs, zero faults |
| **mutant** | `*** NEXUS KERNEL PANIC ***` — `#GP general protection`, vector `0x0D` |

The mutant's panic is the DDR-987 defect by its own signature:

```text
exception: #GP general protection  vector=0x0D
RIP=0xFFFFFFFF80044454        ->  tcp_new_port + 0x2d
RDI=0xDDDDDDDDDDDDDDDD        ->  kheap POISON_FREE, in the first-argument register
RFLAGS=0x86                   ->  IF clear, as the port's own cli leaves it
```

`tcp_new_port` is reached from `tcp_connect` and walks lwIP's pcb lists. Holding
freed-object poison while walking that list is one CPU reading a pcb the other
CPU had already freed — the mechanism DDR-987 named, caught in the act. DDR-987's
original capture carried the same `0xDD` poison in `RAX` rather than `RDI`;
same value, same exception class, different argument slot.

**Speed, which is the whole point.** No `NETHAMMER_PROG` line was emitted at all
before the panic, and progress prints every 1000 iterations — so the mutant
faults in **under 1000 iterations per instance**, against 20,000 clean on the
fixed kernel. Compare the incidental route: `smoke-surfdestroy` surfaces this at
~1 boot in 20. Driving the named mechanism directly is more than an order of
magnitude faster at exposing it.

`ITERS = 20000` is therefore **>20x** the mutant's fault bound, comfortably past
the ~10x §4 asked for. Stated as a bound rather than a point estimate because
that is what was measured: the first fault happened somewhere below 1000, and
the instrument cannot say where below.

## 10. What this does and does not close

**Establishes, positively:** the cross-CPU allocate/free race DDR-987 named does
not reproduce under 40,000 directly-driven connect/close pairs on two CPUs,
where removing the fix reproduces it in under 1000. That is the evidence DDR-987
§5 said was missing and that no amount of `smoke-surfdestroy` sampling could
supply — the difference between failing to disprove and demonstrating.

**Does not establish:** that OPEN-1's `#PF` was this defect. OPEN-1's artefact is
a page fault; this one and DDR-987's are `#GP`. DDR-985 refuted its own Claim A
on exactly this point and it has not been re-established since. The honest
position is unchanged and is now backed by better evidence on one half of it:

- the lwIP use-after-free: **root-caused, fixed, and now positively proven**;
- OPEN-1's `#PF` being that same defect: **still a hypothesis**.

What has changed is that OPEN-1 no longer has a *known live* cross-CPU
use-after-free sitting behind it. Combined with 20/20 on `smoke-surfdestroy`
(§1), the remaining possibilities are that OPEN-1 was this defect and is now
gone, or that it is something else not yet seen. Neither is proven; the second
is the one to watch for, and `[hb]`'s `net_skip`/`net_rxdrop` counters (DDR-988
§5) will exonerate or implicate lwIP immediately if it recurs.

**Recommendation on the tag:** this closes the DDR-987 question the hold was
waiting on, but it does not close OPEN-1 by itself, and the operator's hold was
placed on the `#PF`. That decision stays with the operator.

## 11. Confidence campaign on the fixed kernel

`smoke-nethammer` x5, kernel `2dce56527cd84d5c`: **5/5 pass, 0 fail.**
Cumulative: **200,000 connect/close pairs** across two CPUs, zero faults.

A hash note, because it matters and would otherwise look like an inconsistency:
§9's mutation pair was `abb6b8f582727b6e` (fixed) vs `ad3686405d4912d0`
(mutant). This campaign ran on `2dce56527cd84d5c`, which is `abb6b8f5` **plus
the per-instance self-assertion added to the probe** after the mutation check —
a ring-3 change inside `nethammer.c` only. No kernel or lwIP code differs
between `abb6b8f5` and `2dce5652`, so the mutation result carries over intact.
The self-assertion was added because `EXTRA_SENTINEL` can only test that
`conn_err=0` appears *somewhere*, which one clean instance would satisfy while
the other errored out.

**What 5 runs buy here, and why it is not the 20 the §NON-NEGOTIABLE 2 rule
asks of intermittent gates.** That rule exists for gates whose failure is
probabilistic per boot; this one's is not. The mutant faults in <1000 iterations
of 20,000, i.e. the detector fires in essentially every run where the defect is
present — §9 is the measurement of that. So repetitions here are not raising
the chance of *catching* a 1/20 event; they are guarding against a rarer
interleaving than the one the mutant exposes. 5 x 40,000 pairs is a reasonable
budget for that, and the gate runs on every CI shard-3 boot thereafter, which is
where the long-run sampling actually accumulates.

## 12. OPEN-1 is under-specified — it is at least THREE signatures, not one

Checking `docs/NEXT_TASK_QUEUE.md`'s OPEN-1 entry against DDR-985 turned up
something that changes what "closing OPEN-1" even means. DDR-985 §-table and its
line 136 distinguish **two** failure routes itself, and this DDR adds a third:

| # | route | artefact | where seen |
|---|---|---|---|
| 1 | **hang**, no panic at all — stops after `SYSFSTAT OK`, next expected is `SYSREAD OK`, i.e. inside `sys_read` -> `vfs_read` -> SFS | *no exception* | CI |
| 2 | ring-0 **`#PF`** page fault, much later, after `[sfs] 64K write/read byte-exact OK` | `#PF` | local, 1/20 (DDR-985) |
| 3 | ring-0 **`#GP`**, `RAX`/`RDI` = `0xDDDDDDDDDDDDDDDD` in `tcp_output`/`tcp_new_port` | `#GP` | DDR-987 capture; reproduced on demand by this probe's mutant (§9) |

"OPEN-1" names a *gate symptom* — `smoke-surfdestroy` misses
`PRADYOS_SURFDESTROY_CHURN_OK` — and at least three distinct mechanisms can
produce that symptom. Treating it as one defect is what let DDR-985's Claim A be
written in the first place, and it is why the "is it fixed?" question keeps
resisting a clean answer.

**Consequence for the release decision, stated plainly:**

- Route 3 is now **positively closed** (§9): proven present without the lock,
  proven absent with it, on demand rather than by sampling.
- Route 2 has **not reproduced in 20/20** on the fixed kernel (§1) — evidence,
  at ~64% power, not proof.
- Route 1 is a **hang with no panic**, so neither this probe nor any panic-based
  detector addresses it, and `20/20` says only that it did not recur in twenty
  boots. The queue's own note says to re-check whether it still reproduces
  before instrumenting; §1's campaign is that re-check, and it did not.

So the hammer does **not** close OPEN-1, and no amount of hammering could:
it is the wrong instrument for a hang. What would settle route 1 is a watchdog
on the `SYSFSTAT OK` -> `SYSREAD OK` transition, which does not exist. That is
the honest next instrument, and it is not built.

**Do not record OPEN-1 as closed on the strength of §9.** It closes one of three
routes, and the one it closes is the one that was never OPEN-1's own artefact.


---

## 13. §13 — the gate proved two PIDs, not two CPUs (CodeRabbit, PR #14)

The review found a real gap and it is the one that mattered most in the batch:

> `EXTRA_SENTINEL` checks only whether each pattern appears. […] `QEMU_SMP=4`
> only enables four vCPUs; the gate does not record the CPU used by each worker.

Half of it was already answered — the recipe counts **distinct PIDs** and
requires exactly 2, so "one completion passes" is not reachable. The other half
stood, and it is the load-bearing half:

**Two distinct PIDs prove two instances FINISHED. Nothing proved they ever ran
on different CPUs.** `QEMU_SMP=4` offers four vCPUs; it does not place threads.
This DDR's entire claim is a **cross-CPU** use-after-free — one CPU walking
`pcb->unsent` inside `tcp_output` while another frees it. Two instances that
happened to share one CPU cannot exercise that race at all, and the gate would
go green having tested nothing. "A validation gate that may provide false
confidence in the race fix" is a fair description of that, and it applies to the
strongest positive result in this file.

### 13.1 The fix, and why CPUID rather than a syscall

Each instance ORs its own CPU's bit into `cpu_mask` and prints
`cpumask=<n>` alongside its completion line. `tools/qemu_runner/nethammer_check.py`
takes the **union** across instances and requires ≥2 distinct CPUs.

CPUID leaf 1 returns the initial APIC ID in `EBX[31:24]` and **is not a
privileged instruction**, so a ring-3 probe reads it directly — no new NSI, no
kernel change, nothing added to the syscall surface for a test-only need.

Sampled **every iteration**, not once at exit, because a thread can migrate: a
single reading at the end reports where the instance *finished*, not where it
*ran*. A mask accumulates the whole history.

The cap at bit 7 is deliberate: the mask prints as a decimal and `QEMU_SMP` here
is 4, so a wider machine saturates rather than wrapping an APIC id onto another
CPU's bit. Saturating over-reports the CPU count in a way that could only make
the gate *harder* to pass, never easier.

### 13.2 Validated both directions

The checker was tested against synthetic logs before being wired in, because a
checker that can only say yes is decoration:

| input | union | verdict |
|---|---|---|
| `cpumask=2` + `cpumask=4` | `0x6` → 2 CPUs | **pass**, exit 0 |
| `cpumask=2` + `cpumask=2` | `0x2` → 1 CPU | **fail**, exit 1 |

It also distinguishes "completions carry no `cpumask=` field" (an old probe
binary against a new gate) from "no completions at all" — different problems,
and reporting one as the other would send the next reader in the wrong direction.

### 13.3 MEASURED — and the answer is stronger than the assertion required

Kernel **`5349db4d791cc2ab`**:

```
[nethammer] cpumask union=0xf over 2 instance(s) -> 4 distinct CPU(s)
            instance 0: cpumask=0xf (4 cpu(s))
            instance 1: cpumask=0xf (4 cpu(s))
[nethammer] PASS — 2 distinct pids on >=2 distinct CPUs, 40,000 connect/close
```

Not merely two CPUs between them: **each instance individually ran on all four**.
The scheduler migrates them freely, so the two hammers are interleaving across
every CPU pair, which is the strongest form of the cross-CPU exercise this DDR
claims. Before this section that was an assumption; it is now a number in a log.


---

## 14. §14 — the first run of §13 failed, and it exposed an older bug

The cross-CPU assertion's very first run reported:

```
[nethammer] cpumask union=0x8 over 1 instance(s) -> 1 distinct CPU(s)
[nethammer] FAIL — the hammer instances never ran on 2 distinct CPUs.
```

One instance, on one CPU. The pid check had already passed with **two**, so the
two counts disagreed — and the log said why:

```
PRADYOS_NETHAMMER_OK pid=37 iters=20000 conn_ok=20000 conn_err=0 cpumask=8
PRADYOS_NETHAMMER_OK pid=38 iters=20000 conn_ok=20000 conn_err=[user] ELF loaded (embedded); SFS-rooted fsrm probe spawned
```

pid=38's line was **truncated mid-field** by a kernel print landing between this
probe's `conn_err=` write and the value meant to follow it. Not a placement
failure — a measurement failure (§INV.23).

### 14.1 The older bug it exposed

The completion line was **six separate `write()` calls**, so under SMP there
were five windows for another CPU to interleave. That is not cosmetic, because
the gate's `conn_err=0` sentinel is a substring search over the whole log: a
truncated line contributes nothing, and the check still passes on the *other*
instance's copy.

That is exactly the hole this probe's own comment set out to close —

> "The gate can only test that the string `conn_err=0` appears SOMEWHERE, which
> one clean instance would satisfy while the other errored out"

— reopened, not by the logic, but by the console. It had been latent since the
probe was written and nothing had ever surfaced it. The cross-CPU assertion
surfaced it on its first run, before it had produced a single true result.

### 14.2 Fix

A line builder: fields accumulate into a stack buffer and the whole line goes out
in **one** `SYS_WRITE`. Applied to the OK line and to both FAIL lines — a
truncated FAIL is a missed failure, which is worse.

This does not make the console globally atomic; a single long line could still in
principle be split. It removes the five seams this probe was creating itself.

### 14.3 The checker now tells the two apart

Reporting a truncation as "never ran on 2 CPUs" would send the next reader
hunting the scheduler for a console bug. `nethammer_check.py` compares the count
of completion lines against the count carrying `cpumask=`, and when they differ
says so explicitly and prints the offending line. Three-way controls, all
verified before the real run:

| input | verdict |
|---|---|
| `cpumask=2` + `cpumask=4` | pass, 2 CPUs |
| `cpumask=2` + `cpumask=2` | fail, same-CPU |
| complete line + truncated line | fail, **TRUNCATED**, quotes the line |
