# DDR-1047 — Spinlock contention accounting (`lock_stat`)

**Status:** IMPLEMENTED. Instrument, mutation-proven by forcing the dump.
**Deliberately NOT gated** — §9 says why, and it is not an oversight.
**Group A row:** "Spinlock contention instrumentation — `lock_stat` hold-time +
contention counts — gate `smoke-lockstat`."
**Shipped kernel:** `1bdd581fc269516b` (1,175,946 B, `-Werror` clean).
**M1 (forced-dump) kernel:** `d9e97ff0069500e2`.

---

## 1. What the row asked for, and what is built instead

The row asks for **hold time and contention counts**. This ships **contention
counts and WAIT time**, and drops hold time. That is a deliberate narrowing, not
a partial implementation, and the reason is specific rather than a general
appeal to cost:

| quantity | where it is observable | cost on an UNCONTENDED acquire |
|---|---|---|
| contention count | slow path only | **zero** |
| wait time | slow path only | **zero** |
| hold time | acquire **and** release | an `rdtsc` pair, every acquisition |

Hold time requires timestamping the kernel's hottest primitive on every
acquisition whether contended or not. **OPEN-2 is a timing-sensitive AP freeze**,
and DDR-1010 recorded this exact hazard about its own probe — *"it adds work to
the very syscall path where the race lives, so it may perturb what it measures."*
An always-on `rdtsc` pair inside `spin_lock` could move that bug rather than
measure it.

**Opt-in is not the escape.** DDR-1010's other rule, which DDR-1043 then found
to be literally true for the QMP dump, is that an opt-in instrument is
guaranteed OFF in CI — and CI is the only place OPEN-2 has ever appeared. A
`LOCK_STAT_HOLD=1` build flag would be a hold-time instrument that never once
ran where the question is asked.

So: **wait time, not hold time.** Both timestamps live in the slow path, so the
fast path is untouched. And for the question this instrument exists to answer —
*which lock is a wedged AP stuck on?* — wait time is the direct measurement
anyway. **A frozen CPU is one that is WAITING.**

## 2. The fast path is unchanged

```c
static inline void spin_lock(spinlock_t *l) {
    if (__atomic_test_and_set(&l->v, __ATOMIC_ACQUIRE))
        spin_lock_contended(l);
}
```
One test-and-set and a branch — no more than the original `while`-loop emitted
at each call site. All accounting is out of line in `spin_lock_contended()`.

## 3. Side table, not fields in `spinlock_t`

`spinlock_t` is one byte and is **embedded in other structs** (`sched.c:91`,
`virtio_blk.c:66`). Growing it shifts every struct that contains one — and this
kernel has **assembly-visible fixed offsets**: `percpu.h` documents @0/@8/@16 and
@56..@120, static-asserted in `percpu.c` because `syscall_entry.asm` reads them.
A side table keyed by lock address changes no layout at all, so that entire class
of risk does not arise.

The table is **lock-free by necessity** — taking a lock to record lock statistics
would recurse. Insert is a CAS on an empty key; everything else is a relaxed add.
On a lost CAS the slot is **re-checked for this same key** rather than skipped,
or one lock could occupy two slots and both counts would be half the truth.
Bounded at `LOCK_STAT_SLOTS` (32) with an overflow counter, so a kernel with more
contended locks than slots **reports that fact instead of lying**.

Storage is BSS, so it is zero by definition and **§NON-NEGOTIABLE 10's
`kmalloc`-does-not-zero trap does not arise** — no initialiser is owed anywhere.

## 4. Where it prints, and why only there

`lock_stat_dump()` is called from **one place**: the `[apfreeze]` path in
`ap_freeze_probe` (`kernel/idt.c:250`). Confirmed by disassembly — exactly one
`callq <lock_stat_dump>` in the shipped kernel.

An `[apfreeze]` says a CPU stopped making progress; it does **not** say what it
was waiting for. That is the moment the data exists to answer the question. A
healthy boot prints none of it, which is DDR-941's rule that an unbounded
instrument slows the thing it measures, and DDR-1029's rule that output nobody
asserts on is not free.

Slots print in **slot order, not sorted**: this runs on the freeze path, and a
sort is work added at the worst possible moment for the sake of presentation.

**Once per boot.** That relay loop runs *per frozen CPU*, so a 4-CPU freeze would
print four copies of a table that is **global, not per-CPU** — four identical
answers to one question, on the path where a log most needs to stay readable
(DDR-1043's whole subject). The first freeze also carries the causal information;
later ones are downstream, the same reading DDR-1019 established for panics. The
cost, stated rather than overlooked: a second dump's **delta** — which lock kept
accumulating while a CPU was stuck — is not available.

## 5. M1 — the forced proof

The dump cannot be reached on a healthy boot, so the instrument is proven by
**forcing the branch**, exactly as DDR-1030 proved its `idle2=` field.

M1 adds one temporary line to the `[hb]` heartbeat (`idt.c`, after
`console_line_unlock(hbfl)`):

```c
if (now == 5000) lock_stat_dump();   /* DDR-1047 M1: FORCED PROOF, temporary */
```

Kernel `d9e97ff0069500e2`, `-smp 4`, boot reached `[hb] t=5000`, rc=0.
Addresses resolved against **that same binary**, per §NON-NEGOTIABLE 18:

| lock | hits | waitavg (cycles) | waitmax (cycles) |
|---|---:|---:|---:|
| `g_sched_lock` | 1,902,380 | 16,566 | 74,803,712 |
| `g_rq+0x30` | 4,269 | 3,791 | 5,576,810 |
| `g_rq+0x18` | 3,793 | 4,132 | 5,038,874 |
| `g_rq` | 2,769 | 4,112 | 5,276,894 |
| `g_rq+0x48` | 1,293 | 2,362 | 432,594 |
| `g_rtc_lock` | 936 | 70,292 | 4,239,658 |
| `g_inst+0xC48` | 465 | 62,073 | 1,131,488 |
| `g_inst+0x828` | 154 | 57,732 | 246,616 |
| `g_pmm_lock` | 69 | 88,857 | 1,424,096 |
| `g_surf_lock` | 47 | 9,538 | 181,506 |
| `g_console_lock` | 39 | 1,149,644 | 4,176,648 |
| `g_heap_lock` | 8 | 10,254 | 29,014 |
| `g_inst+0x1068` | 3 | 53,548 | 60,028 |
| `g_net_lock` | 1 | 177,316 | 177,316 |

`overflow=0` — 14 contended locks, comfortably inside 32 slots.

**Reverting M1 returns the kernel to `1bdd581fc269516b` bit-for-bit**, so the
table above is bound to a binary that differs from the shipped one by exactly
that one line.

### 5.1 What the numbers already say

Not conclusions, but the readings that are plainly on the face of it:

- **`g_sched_lock` is the contended lock in this kernel, by ~450x** — 1,902,380
  contended acquisitions in 5,000 ticks, against 4,269 for the busiest runqueue
  lock. Its worst single wait is 74.8M cycles.
- The four `g_rq` entries are the per-CPU runqueue locks (`struct rq` is 0x18 =
  24 bytes), and they are barely contended by comparison. **The per-CPU runqueue
  split did its job; the global lock is what remains.**
- `g_console_lock` is the opposite shape — 39 acquisitions but a 1.15M-cycle
  *average* wait, i.e. a rarely-taken lock held across something slow (the UART).
  Count and wait are independent readings and this row is why both are printed.

**One boot is one sample, and the spread is not small.** An earlier M1 run on a
different binary put `g_sched_lock` at 2,021,160 hits with a 14.65M-cycle worst
wait, against 1,902,380 / 74.8M here — the *ordering* is stable across both runs,
the magnitudes are not. Read the rank, not the digits, unless a specific number
has been reproduced. (That earlier run also showed `g_line_lock` contended once
and no `g_net_lock`; this one is the reverse. Single-hit rows are noise.)

**No fix is proposed from this.** §NON-NEGOTIABLE 3 wants a named mechanism from
a real failing artefact, and a busy lock on a *healthy* boot is neither.

## 6. Counters print at full 64-bit width

`kputdec` takes a `uint64_t` (`console.h:12`). The first draft cast every counter
to `(unsigned)`, which bought nothing and would have **silently printed a wrong
number** once a count passed 2^32 — and `g_sched_lock` reached 2.0e6 contended
acquisitions in 5,000 ticks, so a long boot gets there. Casts removed.

## 7. WHAT IS NOT COVERED — the honest gap, and it is the important one

Two sites take a lock **without** going through `spin_lock`, so neither appears
in the table. Both were found by enumeration (`grep` for
`__atomic_test_and_set` / `__atomic_exchange` outside `spinlock.h`), not assumed
absent.

### 7.1 `sched.c:787` — the work-stealing trylock

```c
if (__atomic_test_and_set(&q->lock.v, __ATOMIC_ACQUIRE))
    continue;                    /* busy — don't convoy, next victim */
```
A **trylock**. It never waits, so there is no wait time to measure. Correctly out
of scope: recording it would be a contention count with no wait behind it, a
different quantity sharing a column. Consequence, stated: runqueue-lock
contention *from the steal path* is invisible here.

### 7.2 `vfs.c:34` — `mnt_lock`, and this one matters

```c
while (__atomic_exchange_n(&m->busy, 1, __ATOMIC_ACQUIRE)) {
    yield();
    ...
}
```

`mnt_lock` is **not a `spinlock_t` at all** — it is a sleep-mutex over a bare
`busy` byte. So it is invisible to `lock_stat`.

**That is the lock CLAUDE.md's own Group A row names as the genuinely unbounded
wait, and DDR-994 built its detector for precisely because it sits on the
`vfs_read` path where OPEN-1 route 1's CI captures hang.** An instrument
justified by "which lock is a wedged CPU waiting on?" does not cover the prime
suspect. That is worth saying plainly rather than leaving to be discovered.

**It was not folded in, and the reason is that the two quantities are not
commensurable.** A spinlock wait is *cycles this CPU burned*; a `mnt_lock` wait
is *wall time until acquired*, during which the CPU ran other threads. Putting
both in one `waitavg` column would invite a specific, plausible, wrong
comparison with a real number behind it — the DDR-1042 failure mode exactly. A
separate section with its own units would be correct; it is not built.

What does cover that site today is **DDR-994's `yield_stall_note`/`_done`**,
which is a *threshold* instrument (it prints past `YIELD_STALL_SPINS` /
`YIELD_STALL_TICKS`). It catches the stuck case, which is the dangerous one.
`lock_stat` is *cumulative* and would catch many short waits that never trip a
threshold. They are complementary, and only one of the two exists for `mnt_lock`.

## 8. Not covered: hold time

Dropped by §1. If a future question genuinely needs it (e.g. *which holder* is
making `g_sched_lock` waits long, as opposed to *how long* they are), the
measurement is a hold-time pair on a **named subset** of locks, not on
`spin_lock` in general. Not built, and not needed by anything open today.

## 9. Why there is no `smoke-lockstat`

The Group A row names a gate. **It is not built, deliberately.**

The dump prints only on the `[apfreeze]` path, and **`[apfreeze]` is in
`GLOBAL_FORBIDDEN`** (DDR-981). So a gate asserting on `PRADYOS_LOCKSTAT` output
would have to either:

1. assert on output that appears only in a run that is **already failing** by
   global sentinel — an assertion that is unreachable on every green run, i.e.
   the dead-arm class this project has now recorded nine times; or
2. ship a kernel that calls the dump on a healthy path purely so a gate can see
   it — which is M1, a mutant, not the shipped behaviour.

Neither is a test. The instrument is proven by §5's forced dump on a recorded
hash, which is the same standard DDR-1030 and DDR-1024 met for instruments that
by construction cannot fire on a green boot. **`smoke-lockstat` should not be
created**, for the same reason DDR-1039 recorded that `smoke-readline` should not
be and DDR-1005 that `smoke-vdso-read` should not be.

## 10. Files

| file | change |
|---|---|
| `kernel/lock_stat.h` | NEW — the contract, and §1/§3's reasoning at the point of use |
| `kernel/lock_stat.c` | NEW — side table, `spin_lock_contended`, `lock_stat_dump` |
| `kernel/include/spinlock.h` | `spin_lock` fast path + out-of-line contended call |
| `kernel/idt.c` | `#include "lock_stat.h"`, `s_lockstat_done`, + the single dump call on `[apfreeze]` |
| `Makefile` | `build/lock_stat.o` in `KERNEL_OBJS` + its compile rule |
