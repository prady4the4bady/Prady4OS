= DDR-880 — OPEN-10 and the `-smp 4` flake are ONE defect (items 46 + 47)

**Status:** Accepted — both items documented to their own stated standard
**Date:** 2026-08-09
**Builds on:** DDR-878. Retires the live hypothesis in `BUILD_TRACKER.md` §5.
**Scope:** measurement + `kernel/main.c` (one boot stamp). No behavioural change.

## The finding

OPEN-10 ("SFS B+tree churn") is **not a B+tree bug, and not a distinct defect**.
It is the same lost-thread failure as item 47, observed through a different
sentinel.

## How the measurement got there — including a wrong turn worth recording

A 30-run campaign of `smoke-sfs-btree-smp4` on one pinned SHA reported
`failures=2/30, with_churn_FAIL_signature=2/30`. Taken at face value that would
have been the first local reproduction of the OPEN-10 signature ever — prior
work got 0/20.

**It was a detector bug.** `make` echoes its own recipe, so every log contains
the literal line `FORBIDDEN_SENTINEL="btree churn FAIL"`. Grepping the whole log
for `btree churn FAIL` matches the *harness's own echo* in every failing run.
The tell is that `signature == failures` exactly; a real signature does not track
the failure count perfectly.

Re-classified against the kernel's actual print (`[sfs] btree churn ...`), both
failures show **neither `OK` nor `FAIL`**. The churn probe never ran at all.

This is the third time in this project that a measurement matched on colour
rather than on the thing itself, and the second time in two DDRs. The rule that
keeps catching it: **assert on what the kernel printed, never on a string that
the harness also prints.**

## The signature, and why it unifies the two items

Both OPEN-10 failures, and all three item-47 failures from DDR-878:

- `[boot-stamp] A probe-block-begin` prints.
- `[boot-stamp] B proofs-begin` **never prints.**
- Exactly ~100 further lines of normal boot follow — other threads run, user
  probes spawn and exit, the heartbeat ticks.
- The last thing `fs_test_thread` does is spawn the **ext4-rooted probe**.

`fs_test_thread` runs every proof after that point — the B+tree churn probe,
`rqstress_proof`, `blkmq_proof`, `smp_blk_integrity`. When it is lost, *all* of
them silently do not run, and whichever gate happens to assert on one of those
sentinels reports "required pattern not found". OPEN-10 and item 47 are two
gates watching the same thread die.

That also explains the thing that never fit: OPEN-10 was named for a B+tree and
`sfs.c` has no global mutable state to race on. There was nothing to fix there
because the B+tree was never the problem.

## Measured reproduction rates, one pinned SHA, gates run individually

| Gate | Rate | Signature |
|---|---|---|
| `smoke-sfs-btree-smp4` (item 46) | **2 / 30** (6.7%) | stamp B absent |
| `smoke-rqstress-liveness` (item 47) | **2 / 40** (5.0%) | stamp B absent |
| `smoke-msixap`, `smoke-blkmq`, `smoke-blk-integrity` | 0 / 8 each | — |

The two rates agree, as one defect seen through two gates should.

Note `smoke-sfs-btree-smp4` already runs at `TIMEOUT_S=180`. The 90 s-window
explanation in `BUILD_TRACKER.md` §5 was correct for the *timeouts* it was
measuring then, but it does not cover these: a thread lost at t≈240 is not
waiting for more time.

## Retiring the B#3 hypothesis

`BUILD_TRACKER.md` §5 records a live hypothesis that OPEN-10 is B#3 (virtio-blk)
seen through the SFS probe. DDR-878 ruled the block layer out: the block gates
are 0/8, the wait-list precondition witness never fires, and the statement after
the stall point is an *embedded* ELF load with no disk I/O. The hypothesis is
closed, not left standing.

## What is narrowed, and what is next

The loss window is between stamp A and stamp B — roughly 90 lines of boot. This
DDR adds **stamp C** immediately after the ext4-probe block:

- C prints, B does not → the thread is lost *after* the ext4 block, and the next
  `elf_load` is the suspect.
- C never prints → it is lost *inside* the ext4 block.

One bit, and it costs one line per boot. It is deliberately landed **without**
the campaign that reads it, because a 30-run capture is ~40 minutes and claiming
a result from a stamp nobody has caught yet would be exactly the unsupported
conclusion this pair of DDRs exists to avoid.

The mechanism remains a per-CPU runqueue / work-stealing loss (rq-1's `rq_next`
/ `rq_on` / `on_cpu` interaction) — one runnable thread lost while every other
thread continues.

## Status of both items

**Item 46 — documented**, to its stated standard: root cause is not a B+tree
bug, the name is a misnomer, the reproduction rate is 2/30 measured, and the
signature is identified.

**Item 47 — documented**, per DDR-878, with the additional finding here that it
is the same defect as 46.

**Item 50** is blocked by exactly one defect rather than two open issues. At
~6% on the affected gates, three consecutive full-suite greens remains plausible
per attempt rather than a wall.

Zero warnings under `-Werror`.

---

## Addendum (2026-08-09) — two more observations, and a push-procedure change

Two further CI failures, both on `main`, both with the DDR-880 signature:

| Commit | `dev/phase1` | `main` | Failing gate |
|---|---|---|---|
| `d5cdf7e` (docs + stamp C) | ✅ | ❌ | `smoke-blkmq` — `'[blk] multi-inflight OK'` not found |
| `aef693e` (item 34) | ✅ | ❌ | `smoke-msixap` — `'[blk] msix on AP OK'` not found |

`d5cdf7e` was **documentation plus one boot stamp**. The identical tree passed on
the other branch. Both missing sentinels belong to proofs that `fs_test_thread`
runs, so both are the same lost thread — this is the unification in section 3
reproducing in CI rather than under a local loop.

**The two branch runs always execute concurrently.** Every pair in the last five
pushes started within 2–3 seconds of the other, because pushing `dev/phase1` and
fast-forwarding `main` in one step fires both workflows at once. That doubles
the shard jobs racing on the runner pool — 12 instead of 6 — on a defect that is
timing-sensitive by nature.

**What is fact and what is not.** The concurrency is fact and directly
observable. "`main` loses more often" is **not** established: two failures out of
five pairs both landing on `main` has p ≈ 0.25 under a fair coin, which is not
evidence of anything. The procedure change below is justified by the contention,
not by that split.

**Procedure change:** push `dev/phase1`, wait for it to go green, and only then
fast-forward `main`. This halves the concurrent load and gives item 50's
"3 consecutive greens on one tip" a materially better chance per attempt. It
costs one CI cycle of wall-clock per push, which is the correct trade when the
alternative is a promotion criterion that keeps failing for a reason unrelated
to the change under test.
