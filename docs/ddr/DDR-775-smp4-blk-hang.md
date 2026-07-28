# DDR-775 — `-smp 4` boot hang narrowed to the virtio-blk completion wait

**Status:** **investigation / findings — no code this slice.** The subsystem is
now identified with evidence and two structural hazards are documented, but the
exact trigger is **not** yet proven, so no speculative concurrency fix was shipped.
**Master-doc reference:** `docs/AETHER_MASTER_FEATURES.md` **Section B, item 3**
(`-smp 4` percpu-sched race root-cause).

## Evidence

| Run | Gate | Mode | Result |
|---|---|---|---|
| 30151522978 | `smoke-surfdestroy` (q35 `-smp 4`) | 180 s timeout | HUNG after `SYSFSTAT OK`; missed the **first** sentinel |
| 30155872016 | `smoke-blk-integrity` (q35 `-smp 4`, **concurrent read data-verify**) | 180 s timeout | missed `[smp] blk integrity OK` |
| local ×3 | `smoke-surfdestroy` | — | **3/3 PASS** |
| 30158060606 | `smoke-blkmq`, `smoke-blk-integrity`, `MSI-X-on-AP`, `smoke-surfdestroy` | `-smp 4` | **ALL PASS** — same commits, run green end-to-end |

| 30163444702 | `smoke-smpuser` — "user-on-AP" (`-smp 4`, ring-3 thread on a non-BSP CPU) | 180 s timeout | missed `[smp] user on AP OK`; **DDR-776 watchdog SILENT — no `[vblk] stuck` line** |
| **30323686134** | **`smoke-swapgs`** (`-smp 4`, `TIMEOUT_S=120`, FORBIDDEN declared so early exit is OFF) | full 120 s window | missed `[percpu] gs OK (syscall ctx)`. **FIRST VALID DDR-777 READ — see verdict below.** |

## ✅ DDR-777 DISCRIMINATOR READ (2026-07-28, run 30323686134) — verdict **(B)**

The instrumentation planted in DDR-776/777 finally produced a readable failure.
The head commit (`ff6d1d4`) is **Python-only**, so this failure cannot have been
caused by the commit under test — which is itself confirmation that BUG-1 is
intermittent and unrelated to the AETHER layer.

**Evidence, all from the serial dump (not the Makefile echo — verified by
stripping the CI prefix and reading only the region after "Serial output was"):**

| Signal | Observed | Rules out |
|---|---|---|
| `[smp] user-on-AP probe t=152` | **present** | (C) APs never came up |
| `[smp] user on AP OK t=152` | **present — the probe SUCCEEDED** | (C); ring-3 ran on a non-BSP CPU |
| `[hb] t=` | **23 heartbeats, t=500 → t=11500, gaps uniformly 500** | **(A) timer stall — decisively** |
| `[vblk] stuck` | **absent** | see the upgraded inference below |
| `KHEAP PANIC` / double-free | **absent** | this is **not** BUG-0/DDR-790 |
| `[percpu] gs OK` **and** `gs FAIL` | **both absent** | the one-shot probe block never executed at all |

**Verdict (B): the BSP boot thread stopped making progress; the timer did not.**

Every line of userspace output — mode toggle, `PRADYOS_NET_ALLOW_OK`, AETHER agent
spawn/exec/verify/reap, `PRADYOS_BIGWRITE_OK`, `[sfs] btree churn FAIL`, and a
final `[user] sys_exit(0)` — lands **before the first heartbeat at t=500**, i.e.
within the first 5 seconds. After that the dump contains **nothing but 115
seconds of perfectly uniform heartbeats**. `kmain` never reached whatever spawns
the ring-3 program that calls `sys_getpid`, so the gate's sentinel could not be
emitted. Neither branch of the probe printed, which is what proves the block was
never entered rather than having run and reported failure.

### The negative that was previously uninterpretable is now admissible

DDR-776 recorded that watchdog silence "cannot distinguish *no stuck request*
from *the watchdog never ran*", because both ride the same timer path. **That
ambiguity is now resolved by the heartbeat**: it is emitted from the same
`irq_timer` path at `(g_ticks % 500) == 0` and fired 23 times, so the watchdog's
`(g_ticks % 100) == 0` scan provably ran ~115 times. Its silence is therefore
real evidence: **no virtio-blk request was stuck**. That removes virtio-blk from
the suspect list on evidence rather than by assumption.

### What this leaves, and what it does not

Still open: **why the BSP stopped progressing** while remaining schedulable
enough for the idle path to run. The last kernel-side self-test region reached is
the SFS churn/GC block in `kmain` (`kernel/main.c` ~1358–1392), and the churn
reported **FAIL** in this run — an independent signal worth pulling on, though not
yet shown to be causal.

**Not concluded:** no fix is proposed here. Three theories have already been
refuted in this DDR by acting before the mechanism was named, and the discipline
holds — this entry records a *reading*, not a remedy. The next step is a
BSP-liveness marker in the `kmain` self-test region (same opt-in pattern as
DDR-790's `PIPE_TRACE`, so it cannot evict gate markers from the log ring).

## ⚠⚠ SECOND CORRECTION (2026-07-25, later) — a UNIFYING hypothesis: the timer stalls

Re-reading run 30163444702's log more carefully **invalidates part of the first
correction below, and points at a single cause for all four failures.**

`[smp] user on AP OK` appears exactly **twice** in that log — once in the gate's
`EXTRA_SENTINEL` echo and once in the `required pattern … not found` message.
**Neither occurrence is serial output.** The serial printed **neither
`[smp] user on AP OK` nor `[smp] user on AP FAIL`**, while the immediately
preceding proofs (`[smp] ap preempt OK`, `[smp] resched OK`) *did* print.

That is decisive, because `smpuser_proof()` (`kernel/main.c:659`) is:

```c
while (!g_user_on_ap && g_ticks < dl)   /* bounded ONLY if g_ticks advances */
    ...
kputs(g_user_on_ap ? "[smp] user on AP OK\r\n" : "[smp] user on AP FAIL\r\n");
```

It is a **deadline poll that must print one branch or the other** — unless
**`g_ticks` stops advancing**, i.e. the timer tick stops being delivered. Then the
"bounded" loop becomes unbounded and nothing is printed at all. That is exactly
what the serial shows.

**This also retracts the inference in the first correction.** I claimed the
DDR-776 watchdog's silence proved "no virtio-blk request was stuck", on the
grounds that "the timer was demonstrably still firing". That was **wrong**: the
boot progress cited (the fuzz test) happens *earlier* than the hang point, so it
says nothing about the timer at the moment of the stall. The watchdog is driven
from the **same timer path**, so if the timer stalls the watchdog simply never
runs — its silence is therefore consistent with **either** "no stuck blk request"
**or** "the watchdog itself never ran", and cannot distinguish them.

**Unifying hypothesis (now leading): under `-smp 4` the timer tick intermittently
stops advancing `g_ticks`.** It explains every observation at once:
- `smpuser_proof()` printing neither OK nor FAIL (its bound never expires);
- the DDR-776 watchdog never firing (same timer path);
- virtio-blk waits never completing (no wake, and no watchdog to report it);
- different gates missing *different* sentinels — whichever `g_ticks`-bounded or
  interrupt-dependent step the boot happened to be in when the timer stalled;
- passing locally and intermittently in CI (a timing-dependent condition).

**Every `g_ticks`-bounded "bounded wait" in the tree is only as bounded as the
timer.** That is a systemic S2 exposure, not one driver's bug.

**Next experiment (decisive, cheap):** prove whether `g_ticks` advances during the
stall — e.g. a heartbeat print driven from the timer path, plus printing `g_ticks`
at entry/exit of the AP proofs. If the heartbeat stops, the timer/LAPIC path under
`-smp 4` is the root cause and the whole B#3 framing moves there.

## ⚠ CORRECTION (2026-07-25) — the virtio-blk narrowing below is NOT supported

The DDR-776 watchdog's **negative** result refutes part of this DDR's conclusion.
In run 30163444702 a *third* `-smp 4` gate failed — `smoke-smpuser`, which is
**not** block-I/O — and the watchdog printed **nothing**, i.e. **no virtio-blk
request was stuck for >5 s**. The timer was demonstrably still firing (the boot
progressed through the fuzz test and a ring-3 thread exited), so the watchdog did
run and its silence is meaningful evidence, not absence of instrumentation.

So the failure is **not** (or not only) a stuck block request. What all failures
share is only `-smp 4`; the missed sentinels differ each time:

- `smoke-surfdestroy` — hung after `SYSFSTAT OK` (next sentinel `SYSREAD OK`)
- `smoke-blk-integrity` — missed `[smp] blk integrity OK`
- `smoke-smpuser` — missed `[smp] user on AP OK`, boot still progressing

**Revised position:** the original Section B#3 framing (an SMP/percpu-scheduler
race affecting AP-dependent proofs) is better supported than the virtio-blk
narrowing. The `sys_read`→virtio-blk inference from the surfdestroy stall point
was a reasonable hypothesis from one data point, but it does not generalise, and
this DDR's "Narrowing" section should be read as **superseded** by this note.

**What survives unchanged:** Hazard 1 (the completion wait is unbounded) and
Hazard 2 (single-element `slot_waiter`) are still genuine defects and still S2
violations worth fixing on their own merit — they are simply **not proven to be
this hang's trigger**.

**Therefore the hang is INTERMITTENT, not deterministic.** Run 30158060606 passed
every gate that failed in the two runs above, including `smoke-blk-integrity`
(which failed in 30155872016) and the `MSI-X-on-AP` test that specifically proves
a blk completion running on a non-BSP CPU. This is consistent with a **rare missed
completion**, not systematic breakage — and it means **one green run cannot prove
a fix**. The honest success criterion is the DDR-776 diagnostic firing (naming the
stuck request) or several consecutive green runs.

**Narrowing:** two *different* gates, both `-smp 4`, both **block-I/O-centric**,
both hanging at the full 180 s bound; three consecutive local runs pass. In the
surfdestroy case the next sentinel after the hang point (`SYSFSTAT OK`) is
`SYSREAD OK`, i.e. the boot stalled inside `sys_read` → `vfs_read` → SFS →
**virtio-blk**. So this is *not* a generic "percpu-sched race" as Section B#3
originally framed it: it is the **virtio-blk completion path under SMP**.

It is also **pre-existing and unrelated to the DDR-774 NVMe work**: the same
`smoke-surfdestroy` gate failed in run 29726803735 (DDR-766), and both failing
gates boot with **no NVMe device**, so `nvme_init()` never runs.

## Hazard 1 (confirmed defect) — the completion wait is UNBOUNDED

`kernel/drivers/blk/virtio_blk.c`, `submit()`:

```c
while (!v->req[s].done)
    sched_block_on(&v->compl_lock);        /* no deadline, no escape */
```

If a completion is ever missed for any reason, the submitter blocks **forever**.
This is a direct violation of **Security Invariant S2 ("bounded everything —
every bound returns an error or a clean kill, never a panic/hang")**: there is no
bound here at all. Independently of what the underlying trigger turns out to be,
this is what converts a transient miss into a permanent 180 s boot hang and makes
the failure undiagnosable — the gate can only report "sentinel not found".

**Note the lost-wakeup race itself is correctly handled** and is *not* the defect:
`sched_block_on()` publishes BLOCKED under `compl_lock`, and `complete()` acquires
that same lock before `sched_unblock()` (the DDR-SMP-3c-locks-4 pattern). So the
classic check-then-block window is closed.

## Hazard 2 (latent defect, probably NOT this trigger) — single-element slot waiter

```c
struct tcb *slot_waiter;      /* ONE submitter waiting for a free slot */
...
v->slot_waiter = current_thread;
sched_block_on(&v->compl_lock);
```

With more than one submitter waiting for a request slot, the second **overwrites**
the first, which is then never woken — a permanent lost wakeup. However
`VBLK_NREQ = 8`, so slot starvation requires **>8 concurrent in-flight
submitters**, which 4 vCPUs are unlikely to reach in these gates. Recorded as a
real defect to fix, but **not claimed as the cause of the observed hangs**.

## Why no code in this slice

The two candidate fixes both touch SMP concurrency in the **shared** block path
that every filesystem gate depends on, and — critically — **the bug does not
reproduce locally (3/3 pass)**, so any change would be pushed unvalidated and
judged only by a ~2.5 h CI run. Shipping a speculative concurrency change under
those conditions is how a one-gate flake becomes a many-gate outage. The evidence
gathered here is the prerequisite for fixing it correctly, not a reason to guess.

## Next slice (specified)

1. **Bound the wait (S2 compliance, do this first).** Replace the unbounded
   `sched_block_on` loop with a deadline-bounded wait so a missed completion
   degrades to an I/O error plus a diagnostic (`[vblk] completion timeout slot=N
   lba=…`) instead of a silent hang. Note this needs either a scheduler
   *timed* block (does not exist today — `sched_block_on()` has no timeout) or a
   bounded `g_ticks` yield-loop; **choosing between those is the design decision
   of that slice** and must be made explicitly, since a yield-loop changes the
   "sleep, never spin" property the current design deliberately has.
2. With the diagnostic in place, the next CI failure names the stuck request, at
   which point the real trigger (missed IRQ vs. lost `virtq_pop_used` vs.
   `head2slot` corruption) becomes decidable rather than speculative.
3. Fix Hazard 2 by turning `slot_waiter` into a proper wait list (or by waking all
   waiters and letting each re-check the loop condition — safe, since a spurious
   wake simply re-tests).

## Architecture prerequisite checklist (for that next slice)

- New NSI/syscalls: none. TCB fields: possibly one (a wait deadline) — decide in
  the DDR. PMM/VMM: none. Capabilities: none. AETHER queue/audit: none.
- **Scheduler hooks: YES, likely** — a timed block would be a scheduler feature;
  this is the crux and must be scoped deliberately.
- Filesystem/root-mount: indirectly (every FS gate rides this path).
- New gate: none needed — `smoke-blk-integrity` and `smoke-surfdestroy` already
  exercise it; the win is that failures become *diagnostic* rather than silent.
- **Security invariants: S2** (the unbounded wait is the violation being fixed)
  and **S6** (fault isolation — a driver-level timeout must fail the I/O cleanly,
  never panic). Nothing touches W^X, NX, or capability contracts.
