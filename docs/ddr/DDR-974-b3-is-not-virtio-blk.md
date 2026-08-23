# DDR-974 — B#3 is not a virtio-blk stall: the backlog row contradicts DDR-878

Status: ACCEPTED (measurement + backlog correction). No behavioural change.
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§INV.4).

`CLAUDE.md` carries B#3 as the release blocker:

> | **B#3 / DDR-806** | `-smp 4` virtio-blk completion stall | timer/IRQ delivery
> under SMP | **MUST be fixed before ISO.** This is BLOCKING the release. |

and Group A repeats it: *"Root-cause the `-smp 4` completion stall per DDR-806."*

**That mechanism was refuted inside this repository on 2026-08-09**, and the row
has been carried forward unchanged ever since. This DDR states what the repo's
own DDRs already establish, adds a current measurement, and corrects the row.

---

## 1. What DDR-878 measured

`DDR-878-slot-wait-list-and-item47.md` §2–§3 did two separable things: it fixed a
real virtio-blk defect (the single `slot_waiter` pointer, replaced with an
intrusive FIFO), **and it ruled that defect out as the flake's cause.** Three
independent results, all in that DDR:

1. **The block layer is clean under `-smp 4`.** On one pinned SHA, run
   individually: `smoke-msixap` 0/8, `smoke-blkmq` 0/8, `smoke-blk-integrity`
   0/8, `smoke-sfs-btree-smp4` 0/8 — **32/32**. Only `smoke-rqstress-liveness`
   failed (1/8). DDR-878's own words: *"The flake is **not** a broad `-smp 4`
   problem."*
2. **The old bug's precondition never occurs.** `submit()` prints
   `[vblk] slot wait list depth>=2` the first time a second submitter queues —
   the exact case the single pointer overwrote. In an instrumented `-smp 4` boot
   it fires **zero times**.
3. **The next step after the stall point does no disk I/O.** The last thing
   `fs_test_thread` does is spawn the ext4-rooted probe — an *embedded* ELF
   load. A block-layer hang cannot explain a thread that stops there.

DDR-878's conclusion, verbatim: *"It is **not wrong data** … and **not a
block-layer hang**."* What it is: *"a single runnable thread lost while every
other thread continues."*

## 2. What the defect actually is, and what happened to it

The real signature (DDR-878 §3, DDR-880):

```text
[boot-stamp] A probe-block-begin   <- prints
[boot-stamp] B proofs-begin        <- NEVER prints
… ~100 further lines of normal boot: other threads run, probes spawn and
   exit, the heartbeat ticks.
```

`fs_test_thread` alone stops. Every other thread is fine.

The investigation then split, and both halves have since landed:

| DDR | what it said | status |
|---|---|---|
| DDR-880 | OPEN-10 and item 47 are one defect (same lost-thread signature) | **corrected by DDR-884** |
| DDR-884 | OPEN-10's true signature is `op=create iter=0` — a *first-create* failure, not a lost thread | superseded by the fix below |
| DDR-964 | root cause: the create-then-init race. `sched_create()` made `fs_test_thread` runnable **before** its caller minted the capability into `->arg`, so a stolen-early thread ran with `CAP_NULL` and its first `vfs_create` returned `-EPERM` (`== -1`) | **FIXED** |

DDR-964's fix is at `main.c:2545` and its comment names this exact spawn:

```c
/* DDR-964 §5: this is the OPEN-10 spawn. Created BLOCKED so ->arg holds the
 * capability before any CPU can enter fs_test_thread — the cli above masks
 * only the BSP's timer, and the boot log shows 796 cross-CPU steals. */
struct tcb *fst = sched_create_blocked(fs_test_thread, 0, "fs");
```

**This matters for B#3 directly.** "A thread made runnable before its state was
initialised, on a kernel doing 796 cross-CPU steals" is precisely the shape that
produces "one thread lost while everything else continues" — item 47's signature.
Whether DDR-964 closed item 47 as well as OPEN-10 is a question the fix's own
mechanism makes plausible but does not settle, so §3 measures it rather than
assuming it.

## 3. Current measurement

Kernel under measurement (R1): `build/kernel.bin`, 1,061,246 B,
`sha256 ab00c00c05fb6fb5e369c0841960f8ef6aa4b16054af3e55527824169f004ea9`.
Stray-QEMU pre-flight per §INV.3 before each campaign: no match.

| gate | runs | result |
|---|---|---|
| `smoke-smpuser` | 1 | PASS — `[smp] user on AP OK`, `HELLO FROM RING-3`, `PRADYOS_MUSL_OK` all present |
| `smoke-smp` (`QEMU_SMP=4`) | **20** | **20 pass / 0 fail** |
| `smoke-rqstress` (`QEMU_SMP=4`) | 20 | _pending — §3.1_ |

Note on the third row: DDR-878's 2/40 baseline was measured on the
**`BSP_LIVENESS`** build (`smoke-rqstress-liveness`, which rebuilds the kernel
with the per-iteration churn witness and restores the clean image afterwards).
The campaign here runs the **canonical** kernel instead — the one that actually
ships — so the rates are not directly comparable and the table says which is
which. The lost-thread signature is visible either way, because the
`[boot-stamp]` prints are unconditional.

### 3.1 Result — 20/20 pass, and the lost-thread signature did NOT recur

| gate | runs | result |
|---|---|---|
| `smoke-rqstress` (`QEMU_SMP=4`, canonical kernel) | **20** | **20 pass / 0 fail** |

Signature check on all 20 kept serial logs (`build/gatelogs/rq_serial<N>.log`),
not just the gate's exit code:

```text
run  1..20   A=1  C=1  B=1   [smp] rqstress OK = 1     (all 20, no exceptions)
runs deviating from A=1 C=1 B=1 rqstressOK=1 : 0
churn FAIL      0/20
rqstress FAIL   0/20
[BUG]           0/20
```

**Item 47's lost-thread signature (`A` present, `B` absent) did not occur once in
20 runs.** Every boot reached all three stamps and printed the proof. Against
DDR-878's 2/40 (5%) baseline on the `BSP_LIVENESS` build, 0/20 on the canonical
kernel is consistent with DDR-964 having closed item 47 along with OPEN-10 —
though 0/20 alone would occur ~36% of the time even at an unchanged 5% rate, so
this supports that reading without establishing it. It is **not** grounds to
call item 47 fixed; it is grounds to stop treating it as the active blocker.

**But the same 20 logs contain something this DDR did not go looking for**, and
it changes §5 — see the correction there and DDR-976.

Per-run serial logs are kept at `build/gatelogs/rq_serial<N>.log`. For any
failure, the diagnosis is already instrumented and needs no new probe:

- **`[boot-stamp] A` present, `B` absent** → item 47's lost-thread signature.
- **The last `[boot-load] <name> t=<ticks>` line names where it stopped.** That
  instrument (DDR-886, `main.c:505`) prints on entry to every SFS-backed load,
  so the ~440-line window between the last spawn and stamp C is already narrowed
  to a single named load.
- **`[sfs] churn FAIL op=… iter=… rc=…`** → OPEN-10's signature instead, which
  would mean DDR-964 did not hold.

## 4. Corrections this DDR calls for — NOT YET APPLIED

**Status of this section: pending.** Two separate reasons, kept separate:

- **Edit 4 touches `kernel/main.c`**, which is a prerequisite of `kernel.bin`.
  Applying it mid-campaign would rebuild the kernel under the §3 measurement, so
  the runs already banked and the runs still to come would be testing two
  different binaries. A comment-only change still relinks and still changes the
  hash, and R1 requires a single kernel hash per measurement.
- **Edits 1–3 are docs and could land now**, but edit 1 rewrites the B#3 row's
  *status*, which is what §3.1 decides. Splitting them would put a row claiming
  an outcome into the tree before the evidence for it exists.

All four land in the same commit as §3.1's numbers, once the campaign finishes.

1. **`CLAUDE.md` B#3 row.** "virtio-blk completion stall / timer/IRQ delivery
   under SMP" is refuted by DDR-878 §2. Re-stated as item 47's lost-thread
   signature, with DDR-964 named as the likely closure and §3's numbers as the
   evidence. To correct once §3.1 lands — the row's *status* depends on the
   measurement, even though its *mechanism* is already refuted by DDR-878.
2. **`docs/BUILD_TRACKER.md:119`** carries the same refuted mechanism
   (*"`-smp 4` virtio-blk completion stall | timer/IRQ delivery under SMP"*)
   even though §5 of the same file already says DDR-878 closed it. To correct.
3. **`docs/PRADYOS_MASTER_PLAN.md` TASK 4** instructs the next session to insert
   `kprintf("[tick] g_ticks=%lu …")` at two line numbers. **`kprintf` does not
   exist in this kernel** — the console API is `kputs` / `kputdec`. A session
   following that task literally does not compile. The line numbers are also
   from a much older `main.c`. To rewrite, pointing at the live instruments.
4. **`main.c:505`'s comment** still frames item 47 as *"a missed virtio-blk
   completion under `-smp 4` would park it here forever"*. That is the refuted
   hypothesis, in the one place a future reader is most likely to trust it. The
   instrument itself is useful and stays; only the framing changes. To correct.

## 5. What would reopen a virtio-blk diagnosis — **IT HAS BEEN REOPENED**

> **CORRECTION, same session, a few hours after this DDR was committed.** This
> section originally read: *"A `[vblk] slot wait list depth>=2` line, a
> `[vblk] compl wait timeout` or `[vblk] slot wait timeout` … **None of those has
> been seen.**"* That last sentence was **wrong**, and it was wrong at the moment
> I wrote it — I had not looked. The §3 campaign then looked, and found
> `[vblk] compl wait timeout` in **17 of 20 runs, 301 occurrences in total**.
> The condition this section set for reopening a virtio-blk diagnosis is met.
> See **DDR-976** for the measurement and what it does and does not show.

The trigger conditions, restated: a `[vblk] slot wait list depth>=2` line
(**still 0/20** — DDR-878's precondition witness genuinely does not fire), a
`[vblk] compl wait timeout` (**FIRING — 17 of 20 runs affected, 301
occurrences in total**; the two numbers are different quantities and this line
used to run them together as "301/20 runs", which reads as an impossible rate), a
`[vblk] slot wait timeout` (**still 0/20**), or a stall whose last `[boot-load]`
line names a load that is still mid-I/O.

**What §1–§4 of this DDR still hold.** DDR-878 ruled out *one specific defect* —
the single `slot_waiter` pointer — as the cause of the *lost-thread flake*, and
that ruling stands: the precondition witness fires zero times here too, and the
lost-thread signature did not recur in 20 runs (§3.1). What DDR-878 never
measured, and what this DDR wrongly generalised from it, is **the completion
timeout rate on an ordinary `-smp 4` boot**. "The block *gates* are green" and
"block I/O never times out" are different claims. The first is true; the second
is false.

So the B#3 row's *mechanism* is no longer refuted — it is, on this evidence,
closer to right than the correction I was in the middle of making. §4's planned
edits are withdrawn pending DDR-976.
