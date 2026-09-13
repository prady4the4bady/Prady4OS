# DDR-1060 — `lock_stat` records only CPUs that eventually ACQUIRE, so it is blind to the frozen one

**Status:** IMPLEMENTED + forced-proof (no gate — see §8)
**Date:** 2026-09-03
**Relates to:** DDR-1047 (the instrument), DDR-1006 §7 (which named this as
OPEN-2's next step), DDR-994 / PRE_LAUNCH_CHECKLIST §4.11 (the `mnt_lock` gap),
OPEN-1 route 1, OPEN-2.

---

## 1. The defect, in the instrument rather than the kernel

`spin_lock_contended()` (`kernel/lock_stat.c:50`, pre-fix) is ordered:

```c
uint64_t t0 = ls_rdtsc();
while (__atomic_test_and_set(&l->v, __ATOMIC_ACQUIRE))   /* <-- spin */
    __asm__ volatile("pause");
uint64_t waited = ls_rdtsc() - t0;
struct ls_slot *s = ls_slot_for((uint64_t)(uintptr_t)l); /* <-- record */
```

**The slot is claimed and every counter written only AFTER the lock is
acquired.** A CPU that never acquires it never reaches those lines. So every
number in the table is contributed by a CPU that eventually *won* — and a wedged
AP, which by definition never wins, contributes **nothing at all**.

`lock_stat.h`'s own header states the purpose: *"for the open question ('which
lock is a wedged AP stuck on?') wait time is the direct measurement anyway. A
frozen CPU is one that is WAITING."* That reasoning is correct. The
implementation measures **completed** waits, which is the complement of the set
it was built for.

This is the dead-arm class, eleventh-plus instance, and the first one located in
a diagnostic rather than in a gate: an instrument whose only reachable data is
the healthy case.

## 2. Why DDR-1047's proof could not have caught it

DDR-1047's M1 was a forced `lock_stat_dump()` from the `[hb]` heartbeat on a
**healthy** boot — the DDR records it plainly: *"boot reached t=5000,
overflow=0, 14 contended locks"*. Nothing was frozen. On a healthy boot every
waiter eventually acquires, so the ordering defect is invisible: the table is
fully populated and looks exactly right.

The proof was of the *plumbing* (counters wired, addresses resolve, no
narrowing) and it was sound for that. It was not, and could not be, a proof that
the instrument answers its stated question, because the scenario in the question
never occurred in the run.

**Carry this:** a diagnostic proven only on the healthy path is proven only for
the healthy path. DDR-1047 §M1 chose a healthy boot deliberately, to get a clean
recorded kernel hash — the cost of that choice is this defect.

## 3. Second half of the same defect, in the dump

```c
uint64_t hits = __atomic_load_n(&g_ls[i].hits, __ATOMIC_RELAXED);
if (!hits)
    continue;
```

Even with §4's fix, a lock whose *only* interaction is a CPU stuck waiting on it
has `hits == 0` — nobody ever completed an acquisition — and would be **skipped
by the printer**. Both halves must move together or the fix is decorative.

## 4. The fix, and why it needs no CPU identity

Claim the slot and increment a **live waiter count** BEFORE the spin; decrement
after acquiring:

```c
struct ls_slot *s = ls_slot_for(key, LS_KIND_SPIN);
if (s) __atomic_add_fetch(&s->waiters, 1, __ATOMIC_RELAXED);
while (test_and_set) pause;
if (s) __atomic_sub_fetch(&s->waiters, 1, __ATOMIC_RELAXED);
```

A frozen CPU then leaves a permanent **+1 on exactly the lock it is stuck on**,
and the `[apfreeze]` dump reads it out.

**This deliberately identifies the LOCK and not the CPU**, and that is the
design rather than a shortfall. The obvious alternative — a per-CPU
`waiting_on` field — needs to know which CPU is executing, and **both** routes
to that are documented hazards on this exact path:

- `this_cpu()` reads `%gs:0`, and **DDR-1010 caught a broken SWAPGS discipline
  as one of OPEN-2's own producers** — writing through a bad GS pointer on the
  path where that bug lives would corrupt memory while investigating it.
- `lapic_id()` is invalid pre-LAPIC, which is why **DDR-1055 refused a per-CPU
  recursion guard** for the console.

A per-lock count sidesteps both. The information lost is *which* CPU is stuck —
and the `[apfreeze]` line already prints that (`cpu=`, `rip=`, `bt=`), so
nothing is actually lost at the point of use.

**Cost, stated:** `ls_slot_for()` moves ahead of the spin, so the contended path
now pays a bounded scan of ≤32 slots with atomic loads before it starts
spinning. **The fast path is still untouched** — one test-and-set and a branch —
which is the property DDR-1047 refused to give up, and it is unchanged here.

## 5. `mnt_lock`, and the unit boundary DDR-1047 drew

PRE_LAUNCH_CHECKLIST §4.11 records the gap: *"`mnt_lock` is not a `spinlock_t`
at all but a sleep-mutex over a bare busy byte, so the one lock DDR-994 names as
the unbounded wait on OPEN-1 route 1's path is the prime suspect this instrument
cannot see."* DDR-1047 left it out for a specific and correct reason — **a spin
wait is cycles this CPU burned; a `mnt_lock` wait is wall time during which the
CPU ran other threads**, and one `waitavg` column holding both invites a
plausible wrong comparison with a real number behind it.

**That reasoning is preserved exactly, and it is what makes this addable now.**
`waiters` is a **dimensionless count**, so it is commensurable across both;
cycles are not. So `mnt_lock` contributes to `waiters` and to nothing else, and
yield-waits print on a **separate line shape** with no cycle columns at all:

```
PRADYOS_LOCKSTAT lock=0x… hits=… waitavg=… waitmax=… waiters=…   (spin)
PRADYOS_LOCKSTAT yield lock=0x… waits=… waiters=…                (yield)
```

Two units, two lines — never one column carrying both.

## 6. What this does NOT claim

- **No defect in the kernel is fixed and no cause is named.** OPEN-1 and OPEN-2
  are untouched. This changes what a *future* freeze can tell us; it says
  nothing about any past one, and the captures that would have carried the data
  were deleted at the time.
- **It does not establish that a lock is involved** in either open issue. A
  frozen CPU with **zero** `waiters` anywhere is now a real, readable answer —
  and it would mean the freeze is *not* a lock wait, which is information the
  instrument previously could not produce in either direction.
- The `waiters` decrement is skipped on the overflow path (no slot), so a kernel
  with >32 contended locks under-reports rather than mis-reports; `overflow=` is
  already printed.
- A waiter that is killed or unwound between the increment and the acquire would
  leak a permanent +1. **Not reachable today** — the spin loop and `mnt_lock`'s
  yield loop both have no exit but success — recorded rather than guarded,
  because a guard for an unreachable path is untested code on a diagnostic.

## 7. Proof

The dump prints only on `[apfreeze]`, which is in `GLOBAL_FORBIDDEN`, so the
scenario cannot be reached by any green gate. Proof is therefore a **forced M1
on a recorded kernel hash**, the DDR-1047 / DDR-1030 / DDR-1024 standard:

- **M1 (the real scenario, not a synthetic one):** a temporary boot-time arm
  takes a dedicated lock on the BSP, never releases it, and has one AP attempt
  it. That AP genuinely freezes, the existing NMI probe fires `[apfreeze]`, and
  the dump must carry that lock's address with `waiters=1` and `hits=0`.
- **M0 (the pre-fix tree, i.e. today's shipped code) under the same arm** must
  show that lock **absent from the table entirely** — no slot, no line — which
  is the defect stated in §1 and §3 reproduced rather than argued.
- Reverting M1 must return the kernel bit-for-bit.

**M0 is the load-bearing half.** Without it, "the new column prints a number"
and "the instrument now sees the frozen CPU" are indistinguishable.

## 8. No gate, deliberately — do NOT create `smoke-lockstat`

Unchanged from DDR-1047's reasoning and repeated here so it is not re-litigated:
the dump is reachable only on a run that is already failing its gate, so an
assertion on it would either be unreachable-passing on every green run (the dead
arm class — which is, with some irony, exactly the defect this DDR fixes) or
would require shipping M1 as product. Precedent: DDR-1039 for `smoke-readline`,
DDR-1005 for `smoke-vdso-read`.

---

## 9. Companion finding — the campaign tool did not pin the binary, and that voided a live campaign of mine

Found while doing the work above, and recorded here rather than under its own
number because it is small and was caused by this session.

`tools/ci/open10_campaign.sh` invokes `make`, so it **rebuilds whenever a source
file changed** — and I edited `kernel/lock_stat.c` and `kernel/fs/vfs/vfs.c`
while a 30-run `smoke-sfs-btree-smp4` campaign was in flight. Measured
timestamps: the campaign started 22:47:14, the edits landed 22:58:48 and
22:59:11, and `build/kernel.bin` was rebuilt at **22:59:40** — between recorded
runs. So runs 1–3 bound `46016bc8c7c7fa3b` and later runs bound a different
binary, **and nothing in the report said so**. Pooled, the number would have
described no binary at all.

**That campaign is VOID and is not reported.** It is not "5/5 clean"; it is one
count over two binaries, which is not a measurement.

**DDR-1023 already wrote this rule down** — its campaign was "rebuilt bit-for-bit
… and hash-verified before AND after every run" — after a different methodology
defect in which the captures turned out to be `make` output rather than serial
logs. The rule was recorded in prose and **never implemented in the tool**, which
is the failure mode itself: a discipline that lives only in a document is one
every future session has to remember unaided.

Fixed in the tool: the kernel hash is pinned at start, printed into the report
(`kernel_pinned=`), and checked **before and after every run**. A mismatch
**aborts** rather than warns — a campaign's whole value is that every run bounds
the same artefact, so once that breaks there is no partial result worth keeping,
and a warning buried in a 30-run log is a warning nobody reads.

**The operator instruction this serves:** *"keep surfacing gaps like this across
other components too — process gaps, not just code gaps."* This is one, it is
mine, and the tool now enforces what the document only asked for.
