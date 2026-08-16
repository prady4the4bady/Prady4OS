= DDR-942 — the workers are STILL QUEUED; and 11 probes died of `ELF_E_NOMEM`

**Status:** ACCEPTED. Measurement + one instrument. **No fix.**
**Date:** 2026-08-16
**Evidence:** CI run on tip `62c65cd`, shard 0, `smoke-vault` reddened by the
DDR-785 foreign-probe rule.
**Lineage:** DDR-934 → DDR-936 → DDR-940 → DDR-941 → **DDR-942 (this)**.

## 1. The blk workers were created and never executed — confirmed

```
[smp] blk integrity FAIL workers-late done=0x0000000000000000 spawned=4/4 prog=0,0,0,0
```

**`spawned=4/4` retires DDR-934's allocation hypothesis** for this probe. All
four TCBs were created.

**And `done=0x0` excludes allocation failure INSIDE the worker**, which the
`prog=0` field alone could not. `blkint_worker` is:

```c
uint64_t buf = pmm_alloc_page();
int ok = (bd && buf);
for (int i = 0; ok && i < 64; i++) { …; prog[id]++; }
if (buf) pmm_free_page(buf);
__atomic_or_fetch(&g_blkint_done, 1u << (ok ? id : id + 8), …);
```

A worker whose `pmm_alloc_page()` returned 0 would skip the loop (`prog` stays
0) **but still set its bit in the HIGH byte** — `done` would be `0x0f00`, not
`0x0`. `done=0x0` means not one worker reached even its failure path. This
matters because it is the difference between "the threads ran and could not get
memory" and "the threads never ran", and `prog=0,0,0,0` alone is consistent
with both. It is the latter.

## 2. All three strand counters read ZERO — so the thread is STILL QUEUED

Same failing boot: `ubcas=0 ubrq=0 rqmiss=0` on every heartbeat captured.

- `ubcas=0` — no `sched_unblock` CAS ever failed.
- `ubrq=0` — `rq_push` never early-returned on `rq_on`.
- `rqmiss=0` — `rq_take` never dropped a non-READY entry.

Note the blk workers do not even use `sched_unblock`: `sched_create` creates
them `THREAD_READY` and enqueues **directly** (`sched.c:842-844`). So they were
pushed, and nothing removed them.

Enqueued, never dropped, never run. **The only reading left is that the entries
are still sitting in a ready queue that is not being drained.**

Three hypotheses have now been excluded by measurement rather than argument:
the CAS gate, the `rq_on` gate (both DDR-936), and the pick-time drop
(DDR-941). Together with the four refutations of the missing-IPI theory, that
is seven mechanisms killed by instruments.

## 3. Instrument: `rqdepth=` / `rqcpus=`

Total entries across all ready queues, and how many CPUs hold at least one.

**Baseline measured first, on a PASSING boot** (`smoke-wmorder`, tip
`62c65cd` + this instrument):

```
[hb] t=10500 … rqmiss=0 rqmst=0 btnedge=0 rqdepth=6 rqcpus=1
```

**A nonzero depth is the normal steady state.** Six entries on one CPU, stable
across heartbeats, on a healthy boot.

This corrects the reading criteria in this document's own first draft, which
said "depth stays >0 ⇒ the queue is not being drained". That test would fire on
**every healthy boot** — a false positive by construction. Establishing the
baseline before trusting the discriminator is the DDR-910 lesson, and it very
nearly went unlearned here for the fourth time.

Corrected criteria:

- depth **above baseline and monotonically non-decreasing** (e.g. baseline+4
  when the four blk workers strand) ⇒ entries are accumulating and not being
  picked ⇒ the defect is in the pick/steal loop; `rqcpus` says whether they are
  stranded on a CPU that is not scheduling.
- depth **at baseline** while a probe reports "never ran" ⇒ the "still queued"
  reading is **refuted** — the thread left the queue by a path none of the four
  counters observe. That would be a genuine surprise and is worth more than
  another guess.
- The absolute number is meaningless without the same boot's early heartbeats
  for comparison, so always read the **series**, never one line.

Deliberately lock-free and bounded (4096). Taking `q->lock` from the timer ISR
to print a diagnostic would add a real deadlock risk to every boot in order to
observe a rare one; a torn count is acceptable, a hung kernel is not.

## 4. SEPARATE and previously invisible: 11 probes failed with `ELF_E_NOMEM`

DDR-941's naming instrument fired immediately:

```
[boot-load] FAILED INIT.ELF   reason=elf rc=8
[boot-load] FAILED PRISM.ELF  reason=elf rc=8
… 11 in total (CMUSL, DMESG, FPUTST1, FPUTST2, FUZZ, INIT, KILL, PRISM,
                SETNAME, TIME, TLSTEST)
```

`rc=8` is **`ELF_E_NOMEM` — out of frames / address space** (`elf.h:40`).

The immediately preceding green run (`de993b5`) contains **zero** ELF load
failures, so this is not constant background: this boot ran out of frames.

Before DDR-941 these printed `[user] ELF load FAILED rc=8` with **no filename**,
so with ~30 probes booting through one function nobody could tell which failed,
and the condition was never attributed.

**This is NOT asserted to be the same defect** as §1-§2, and §6.0-C applies:
it gets its own root cause. Two facts argue they are distinct — the blk workers'
own `pmm_alloc_page` evidently succeeded (§1: `done=0x0`, not `0x0f00`), and
`kmalloc` (TCB + stack) succeeded 4/4 while the PMM frame allocator was
failing. Different allocators, different outcomes, same boot.

But a boot that exhausts frames is a strong candidate for a **common upstream
cause** of several tracked flakes, and it has been invisible the whole time.
It is now the highest-value open question after the queue-depth readout.

## Next

1. Read `rqdepth`/`rqcpus` on the next failing run.
2. Independently: characterise the frame exhaustion — how many free frames at
   boot, what consumes them, whether the 11 failures are one burst.

## Not doing

No fix. §6.0-B. The tally stands at seven mechanisms proposed-and-refuted
across DDR-920/928/932/934/936/941; every one that was retired was retired by
an instrument, and none by argument.
