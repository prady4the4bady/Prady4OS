# DDR-1156 — virtio-blk slot-wait timeout leaves the waiter linked

**Status: DESIGN committed before code (NON-NEGOTIABLE 5), then BUILT in the same
branch.** Operator-approved (PR #27 comment 5970979125, OWNER, 2026-10-03,
"Fix 1"). **This does NOT close U1, U2, U3 or OPEN-1** and no open issue moves;
it removes one use-after-free reachable from the block path, found by the
operator's code review, not from a hunt artefact.

## §1 The defect, confirmed from the full code

`kernel/drivers/blk/virtio_blk.c` `submit()`, the slot-claim loop: when all
`VBLK_NREQ` (8) request slots are in flight, the caller links `current_thread`
onto the per-unit slot wait list (`slot_head`/`slot_tail`, chained by
`tcb.blk_wait_next`) and calls `sched_block_timeout(&v->compl_lock,
&v->slot_free, 500)`. On the `-ETIMEDOUT` branch the code prints `[vblk] slot
wait timeout`, clears the owner record, unlocks and **returns `-EIO` without
removing `current_thread` from that list** (verified at the timeout branch, the
enqueue being the three lines that set `slot_tail->blk_wait_next` /
`slot_head` / `slot_tail`).

Consequence: the timed-out thread stays reachable from `v->slot_head`. The next
`slot_wake_one(v)` (completion path, or the descriptor-exhaustion path) pops it —
reads `w->blk_wait_next`, writes 0 into it, and calls `sched_unblock(w)`. If that
thread exited and its TCB was freed in between, this is a **use-after-free**: the
links and `sched_unblock` touch freed, PMM-poisoned memory. If instead the thread
retried, it re-enqueues itself and can appear twice.

**Finding 2, confirmed and reported:** `sched_free_tcb` (`sched.c`) unlinks a
dying thread from the all-threads ring and from the per-CPU ready FIFO (DDR-996)
but **not** from any device slot-wait list — `blk_wait_next` is a driver-private
field the scheduler has no knowledge of. **The fix belongs at the driver timeout
site, not in `sched_free_tcb`:** making generic scheduler code walk every
driver's private wait lists would couple it to every driver and need a field per
list. The driver that owns the list removes its own entry. This is recorded as
the answer to the operator's "should `sched_free_tcb` unlink from the slot wait
list" — **no, it should not.**

## §2 The instrument first (NON-NEGOTIABLE 3)

Before the fix, a counter `g_vblk_wake_notblocked` is added in `slot_wake_one`:
it increments when the popped waiter's `state != THREAD_BLOCKED`, i.e. a wake
that pops a thread that is not actually waiting — the exact signature of a stale
or recycled entry. A correct wake only ever pops a `THREAD_BLOCKED` thread, so on
a healthy kernel the counter stays 0. `sched_unblock(w)` is additionally guarded
on `state == THREAD_BLOCKED`, so a stale pop is counted and skipped rather than
dereferenced-and-written — which also makes the gate's mutant fail by the counter
rather than by a crash. This is pure observation plus a safe guard; the counter
is NOT in `GLOBAL_FORBIDDEN` (its non-zero value is a find, read by the gate).

## §3 The fix

A helper `blk_slot_unlink(v, t)`, called under `compl_lock` on the `-ETIMEDOUT`
branch before the unlock/return, walks `slot_head..slot_tail` and removes `t`,
fixing `slot_head`/`slot_tail` and clearing `t->blk_wait_next`. After it the list
can never contain a thread that has left the wait. One call site; the healthy
path is untouched (a thread that gets a slot was never left on the list).

## §4 The gate — force the timeout, assert the list is clean

`smoke-blkslot` (its own boot, `QEMU_PROBES=blkslotwait`, on a disk config whose
LAST virtio-blk unit is a blank scratch disk — never the mounted root). The
probe `vblk_slotwait_selftest()` (in `virtio_blk.c`, which owns the private
struct):

1. Under `compl_lock`, mark all 8 slots of the scratch unit `used=1 done=0
   warned=1 t0=g_ticks waiter=0` — **fixtures**, no device traffic, so `complete()`
   never runs for this unit and the DDR-776 watchdog skips them (`warned`).
2. Spawn a kernel thread that calls the **real** `vblk_read()` on that unit. It
   finds no free slot, enqueues on the slot wait list, and times out after 500
   ticks, returning `-EIO` (DDR-1014's rule: the thread under test runs the real
   `submit()`; only the precondition is a fixture).
3. After the thread reports done (so the timeout branch has run), read the chain
   under `compl_lock`: `clean = (slot_head == 0 && slot_tail == 0)`, then call
   `slot_wake_one(v)` once (a no-op on a clean list) and re-check, then release
   the fixtures.
4. Print `PRADYOS_BLKSLOT clean=<0|1> notblocked=<delta>`.

- **Required:** `PRADYOS_BLKSLOT clean=1 notblocked=0`.
- **Forbidden:** `clean=0`.

Gate cost ~5 s (the 500-tick timeout) plus margin, inside `TIMEOUT_S`.

## §5 Mutation check

**M1 — remove `blk_slot_unlink`** from the timeout branch (the pre-fix tree). The
timed-out thread stays linked: step 3 reads `slot_head != 0` → `clean=0` (gate
FAILS on the forbidden pattern), and the `slot_wake_one` in step 3 pops the
now-departed thread whose `state != THREAD_BLOCKED` → `notblocked=1`. Both
signals fire; either fails the gate. The mutant is literally the current code, so
no synthetic defect is written (the DDR-1066/1067/1090 form).

## §6 Not claimed

- U1/U2/U3 and OPEN-1 do **not** move; no mechanism is named for any of them.
  The U1 `#GP` was at `resolve+0x61` (`cap.c`), a different subsystem; this is a
  separate, independently-found defect on the block path.
- No rate is claimed. The UAF requires a slot-wait **timeout** (all 8 slots in
  flight for 500 ticks) followed by the waiter's exit before the next wake — rare
  in normal operation, which is why no hunt has surfaced it.
- The completion-timeout path (operator finding 3: `head2slot[head]` left set, a
  late device write into a freed/reused buffer) is **not** addressed here; it is
  DDR-1148 §4's counted-not-fixed case and needs its own record.

## §7 Measured result (2026-10-03)

Built warning-clean at `-Werror`; kernel.bin `2d375be6302fb413`, **1,474,954 B —
size unchanged** (the fix fits inside existing page padding), so the CLAUDE.md
size/headroom pair and `ci-docstate-check` are unaffected. One new ELF is NOT
added (the self-test is in the kernel), so `ci-probe-rodata-check` stays at 85.
`ci-shard-check`: 186 → **187 gates** (`smoke-blkslot`, shard 0 strict). All ten
hygiene checks pass.

- **Fixed kernel, `make smoke-blkslot`:** `[blkslot] PRADYOS_BLKSLOT clean=1
  notblocked=0 t9=1` → PASS. `t9=1` is the non-vacuity witness: the 9th submitter
  really ran the real `submit()`, enqueued on the slot wait list, and returned
  `-EIO` from the 500-tick timeout — the path under test was exercised.
- **M1 (unlink + helper removed = the pre-fix tree), same gate:** `[blkslot]
  PRADYOS_BLKSLOT clean=0 notblocked=1 t9=1` → FAIL (forbidden sentinel `clean=0`
  fired). The timed-out thread stayed on the list (`clean=0`), and
  `slot_wake_one` popped that departed TCB whose state was not `THREAD_BLOCKED`
  (`notblocked=1`) — the exact stale-pop this fix removes, observed directly.
- Both land on the same arm (`clean=`) and the secondary instrument
  (`notblocked=`) corroborates the mechanism; `t9=1` in both runs rules out a
  vacuous pass/fail.

Regression on the fixed kernel: `smoke-blkmq` rc=0, `smoke-blk-integrity` rc=0,
`smoke-shell` 5/5 — the one change on the normal wake path (the `THREAD_BLOCKED`
guard in `slot_wake_one`) does not alter behaviour for a correctly-waiting
submitter, which is `THREAD_BLOCKED` when woken, so `sched_unblock` still runs.

## §8 Addendum (2026-10-14, design before code): the no-sleep return also leaves the submitter linked

Review finding (CodeRabbit on PR #29, verified against the source, not taken on
trust). `sched_block_timeout()` returns 0 **without blocking** when `*done` is
already set (`sched.c`: `if (*done) return 0;`). In `submit()` that happens when
`slot_free == 1` on entry to the wait — i.e. a `slot_wake_one()` has set it and
the woken waiter has not yet run its `v->slot_free = 0`. The submitter has just
linked itself at the tail, returns 0, clears `slot_free`, re-scans, finds every
slot still used, and **links itself again**: the second pass sees
`slot_tail == current_thread` and writes `current_thread->blk_wait_next =
current_thread`, a one-node self-loop. On the eventual timeout `blk_slot_unlink`
matches at the head, sets `slot_head = t->blk_wait_next` (= `t` itself) and
`slot_tail = 0`, so the departed TCB stays reachable from `slot_head` — §3's
defect reached by a second route that §3 did not cover.

**Root cause:** §3 placed the unlink on the *timeout* branch only, so it holds the
invariant "a thread that leaves the wait is not on the list" for one of the two
ways of leaving. **Fix:** unlink unconditionally after *every* return from
`sched_block_timeout()`, under `compl_lock` (re-taken by the callee), before the
timeout check. After a normal wake `slot_wake_one` has already popped the thread,
so the unlink walks the list, does not find it and is a no-op; after a no-sleep
return or a timeout it removes the entry. One placement covers all three exits.

**Gate arm (same gate, `smoke-blkslot`):** a second submitter `t10` runs with all
eight slots occupied **and `slot_free = 1` preset** — the no-sleep precondition,
manufactured deterministically. It must still time out with `-EIO` (`t10=1`, the
non-vacuity witness) and leave the list empty (`cleanb=1`). The control is the
§7 tree with only the timeout-branch unlink: predicted `cleanb=0`.

## §9 Measured result of §8 (2026-10-15)

Built warning-clean at `-Werror`; kernel.bin `7020d00b6e717e9c`, 1,479,050 B —
size unchanged against the pre-§8 tree, so the size/headroom pair and
`ci-docstate-check` are unaffected (`docstate: OK`).

- **Control (the §7 tree + arm B, i.e. timeout-branch unlink only), kernel
  `1cd537221bcc78a1`:** `PRADYOS_BLKSLOT_B cleanb=0 t10=1` — the no-sleep return
  left the departed submitter on the list, exactly as §8 predicted. Against the
  gate with arm B wired: **rc=2**.
- **Fixed kernel `7020d00b6e717e9c`:** `PRADYOS_BLKSLOT_B cleanb=1 t10=1` and arm
  A unchanged (`clean=1 notblocked=0 t9=1`) → PASS. `t10=1` is the non-vacuity
  witness: the second submitter ran the real `submit()` and timed out with `-EIO`.
- Regression on the fixed kernel: `smoke-blkmq` rc=0, `smoke-blk-integrity` rc=0,
  `smoke-shell` **5/5**; hygiene ALL TEN.
