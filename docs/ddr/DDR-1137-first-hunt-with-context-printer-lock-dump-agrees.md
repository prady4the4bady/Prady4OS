# DDR-1137 — The first hunt with DDR-1135's printer: 1,100 boots, every DONE line read, and at the vblk freeze site the lock dump now AGREES with the backtrace

**Date:** 2026-09-23
**Status:** Measurement and address resolution. **No code change, no gate.** `kernel.bin`
is **not** rebuilt in the shipping tree (`25f4dae4a3f90bcb`, checked before and after).
**NO FIX, NO MECHANISM NAMED. OPEN-2 DOES NOT CLOSE.** No open issue moves.

**Artefact:** `open2-hunt` run **35885600287**, `workflow_dispatch`, 20 lanes × 55 runs,
`OPEN2_HUNT=32`. Every lane prints `tree_sha=6305e20…` and
`kernel_pinned=ca8107ec7f5d8de7`. `runs=55` fits the 180-min timeout as DDR-1136 §1
required: all 20 jobs finished in about 166 min.

---

## §1 — THE DISCRIMINATOR, THE TIMEOUT CHECK, AND THE DENOMINATOR (every line read)

- **DDR-1132's discriminator.** **4 of 20** lanes are non-success: **0, 4, 9 and 15**. That
  is not all twenty, so this is not a setup failure.
- **DDR-1136's timeout check.** No job was cancelled. **Every one of the 20 lanes printed
  its own `[campaign] DONE` line.** All twenty were fetched and quoted. None is inferred
  from the dispatch parameters, which removes DDR-1134 §1's "unread" caveat for this run.

| lane | own DONE line |
|---|---|
| 0 | `runs=55 signal_runs=1 churn_runs=55 kernel_pinned=ca8107ec7f5d8de7` |
| 4 | `runs=55 signal_runs=1 churn_runs=55 kernel_pinned=ca8107ec7f5d8de7` |
| 9 | `runs=55 signal_runs=1 churn_runs=55 kernel_pinned=ca8107ec7f5d8de7` |
| 15 | `runs=55 signal_runs=1 churn_runs=54 kernel_pinned=ca8107ec7f5d8de7` |
| 1, 2, 3, 5, 6, 7, 8, 10, 11, 12, 13, 14, 16, 17, 18, 19 | `runs=55 signal_runs=0 churn_runs=55 kernel_pinned=ca8107ec7f5d8de7` (each read individually) |

**Totals: 1,100 boots, 1,099 with churn, 4 signal runs, one binary.**
- Lane 15's one NO-CHURN run is its own signal run (§3.2). The check halted the CPU at
  `t=1000`, so this is a consequence of the fire, not a vacuous run (DDR-1128's point).
- DDR-1130's no-filesystem trap is not in play.

## §2 — DDR-1135 DID ITS JOB

This is the first hunt with the context printer. Every signal run reached the job log with
its index, **5 lines of leading and 40 lines of trailing context**, and, for the two
`[apfreeze]` runs, the full `PRADYOS_LOCKSTAT` dump that the old printer could never
show. **No truncation notice fired in any lane**: indexes held 1 to 5 lines against the
cap of 40.

The printer had no panic body to prove itself on: **none of the four signal runs is a
panic**. So DDR-1135's specific promise, a panic body in the job log, is **still
unexercised in the field**. What is exercised is the context and the lock dump.

## §3 — THE FOUR SIGNAL RUNS, BY PRODUCER (not pooled, DDR-1019)

Addresses are resolved against the a390eab `OPEN2_HUNT=32` rebuild (§5), **in python3,
never awk**.

### §3.1 `[schedcheck]` ×2 (the deliberate halt, `schedule_locked+0x671`)

| lane/run | tid | disp − saves | rq_on | r15 | ret | rflags − rsp | depth below ktop |
|---|---|---|---|---|---|---|---|
| 4/25 | 22 | 11 − 9 = **2** | 0 | `…1668D` | `…16DCF` | **0x30** | 2,384 B |
| 15/11 | 22 | 17 − 15 = **2** | 0 | `…1668D` | `…16DCF` | **0x30** | 1,536 B |

- `…1668D` = `finish_task_switch+0xd`, the value the consumed-frame chain predicts.
  `…16DCF` = `schedule_locked+0x6af`, the return after `call local_irq_restore`.
- **Against DDR-1136 §3.1:**
  - **Every fire in this dispatch reads saves+2**, as did every fire in DDR-1136. The
    project has now seen **8** saves+2 fires, and **all 8 read `rq_on=0`**. DDR-1118's
    double-resume precondition is absent every time. (The saves+1 fires, e.g. DDR-1133 §9's
    lanes 5 and 13, are a separate population and are not counted here.)
  - **r15 is now 4 × `…16842` and 4 × `…1668D`.** DDR-1136 §3.2 broke DDR-1134's
    co-occurrence at 2 of 6. Both new fires land on the ordinary consumed-frame value, so
    the split is now **even**. As DDR-1136 §3.2 said, this is **what DDR-1133 §10.3
    reading (ii) predicts** (a lost non-atomic `switches_away++`, with no relation to the
    r15 slot). **It still does not establish (ii).**
  - **rflags = rsp + 0x30 holds on both.** DDR-1134 counted 8 fires, and DDR-1136 added 4
    with one exception, lane 11 (`+0x1C0`). The count is now **13 of 14**, which is still
    not universal.
- **Lane 4's arrival path** is the `[apfreeze]` at the same halt RIP, with `bt` =
  **`schedule+0x11 ← sched_ap_enter+0x178 ← smp_ap_entry+0x30a`**. That is the AP
  bring-up path, the third time it has been seen (DDR-1128/1129, DDR-1134 lane 0).
  - The census `ticks[0=1500,1=1479,2=163,3=1476]` shows **exactly one** frozen CPU
    (CPU 2).
  - The lock dump shows **`g_sched_lock` with `waiters=1`**. **Recorded, not attributed.**
    The census shows the other three CPUs still ticking, so whatever that waiter is, it is
    not a second frozen CPU in this capture.
- **Lane 15** printed no `[apfreeze]`: the capture ends at `t=1000`. **Its arrival path is
  not known.**

### §3.2 `[apfreeze]` at `spin_lock_contended+0x92` ×2, with NO `[schedcheck]`

Lanes 0 (run 15) and 9 (run 55).
- `rip=0xFFFFFFFF80049FB2` = **`spin_lock_contended+0x92`**.
- `bt` = **`spin_lock+0x25 ← spin_lock_irqsave+0x1c ← submit+0x2e ← vblk_read+0x34`**.
- This is **the same site as DDR-1136 §3.4's lane 19** on this binary, and as DDR-1121/1122
  on an older one. That makes **three** on `ca8107ec7f5d8de7`.
- **Both captures show TWO frozen CPUs, and in both the unlatched CPU froze FIRST:**
  - lane 0: `ticks[0=1500,1=164,2=163,3=1476]`
  - lane 9: `ticks[0=1500,1=157,2=155,3=1312]`
  - That is DDR-1122's shape exactly: `s_victim` latches CPU 1, and **CPU 2's RIP is never
    captured**.

**THE FINDING: THE LOCK DUMP NOW AGREES WITH THE BACKTRACE.**
- DDR-1121's contradiction was that the backtrace named a vblk `compl_lock` while every
  `compl_lock` read `waiters=0` and the only live waiter sat on a runqueue lock.
- DDR-1136 §3.4's lane 19 could not test that, because the old printer dropped the dump.
- Here, in **both** captures, **the live spin waiter is on `g_inst+0x828`**:
  - `waiters=1` in lane 0, `waiters=2` in lane 9;
  - every runqueue lock (`g_rq+…`) reads `waiters=0`.
- **Measured in this binary, not carried:**
  - `submit` opens `movq -0x10(%rbp),%rdi; addq $0x408,%rdi; callq spin_lock_irqsave`,
    and `submit+0x2e` (`…219EE`) is that call's return address.
  - The `struct vblk` stride is **`imulq $0x420`** (three sites).
  - `g_inst` = `0xffffffff80159b10`, so `+0x828` = `g_inst[1] + 0x408` = **unit 1's
    `compl_lock`**.
  - Units 2 and 3's `compl_lock`s (`+0xC48`, `+0x1068`) read `waiters=0`.
- So in these two captures **the backtrace (a `compl_lock`) and the table (unit 1's
  `compl_lock` has the live waiter) point the same way**. DDR-1121's four propositions
  are **consistent here**.
- **This does not retroactively resolve DDR-1121's capture.** That was a different binary
  and a different boot, and its disagreement stands as recorded.
- Lane 9's `waiters=2`, with two frozen CPUs, is **consistent with both frozen CPUs
  spinning on unit 1's `compl_lock`**. If that is so, **the holder is not a frozen CPU**.
  Lane 0's `waiters=1` leaves CPU 2 elsewhere. **Neither reading is asserted.**
  - CPU 2's RIP is the one datum that would settle it, and this detector cannot capture
    it (DDR-1122).
  - A matching shape is not a mechanism (DDR-1056).
- **A separate observation, not attributed:** the `[vblk] compl wait timeout` lines in
  both captures name **unit 3** (and unit 0 in lane 9), **not unit 1**. Their `dest_cpu`
  is 1, the latched frozen CPU.
- **Not common to both, so not attributed:** lane 9's heartbeats after the freeze read
  `calls≈bails` (481/481, 750/749, 643/643; `max` spins ≈2–3 M on cpu 3). Lane 0's read
  `calls=0 bails=0`.
- Both captures also carry `yield lock=g_mounts+0x1C waiters=3/2` (mnt_lock). This is
  DDR-1122 §4(b)'s observation again. **Recorded, not attributed. OPEN-1 does not move.**

## §4 — NO POOLED RATE

There are two producers here: `[schedcheck]` ×2 and the vblk `compl_lock` freeze ×2.
- They are not the same event and are **not pooled** (DDR-1019, DDR-1042). DDR-1132 §5's
  figure is not revised.
- For the record only, on one binary: `[schedcheck]` **2 in 1,100**; the vblk site
  **2 in 1,100** (3 across DDR-1136 and here).

## §5 — ADDRESS RESOLUTION (§INV.18)

- a390eab was rebuilt at `OPEN2_HUNT=32` in a **detached scratchpad worktree**. It
  reproduces **`ca8107ec7f5d8de7` bit for bit** at 1,319,306 B, and the build is
  warning-clean (0 `\b(error|warning):` matches).
- musl and lwip were linked from the main tree. `git ls-tree` shows the submodule pointers
  **identical at a390eab and HEAD**; the matching hash is the proof.
- The binary holds exactly one `call context_switch` (checked again).
- The worktree was **removed**. The shipping `build/kernel.bin` read `25f4dae4a3f90bcb`
  before and after. **No QEMU was run.**

## §6 — NOT CLAIMED

- **NO fix and NO mechanism. OPEN-2 does not close.**
  - §3.2's agreement makes the vblk site **internally consistent for the first time**. It
    does not say **who holds unit 1's `compl_lock`**, or why.
  - §3.1 adds two fires to a distribution; it does not add a mechanism.
- **Nothing is withdrawn.** DDR-1121, DDR-1122, DDR-1134 and DDR-1136 all stand.
  - DDR-1121's disagreement stands for its own capture.
  - DDR-1136 §3.2's broken co-occurrence is only reinforced.
- **No defect is alleged** in `spin_lock_contended`, `lock_stat.c`, `virtio_blk.c`,
  `submit`, `switch_wait_offcpu_sched`, `finish_task_switch` or the campaign script.
- **DDR-1135's panic-body promise is still unexercised in the field** (§2).
- **No code change, no gate, no new sentinel.** `GLOBAL_FORBIDDEN` is 77, 179 gates.
  `kernel.bin` is unchanged at `25f4dae4a3f90bcb`, so the size/headroom pair and
  `ci-docstate-check` are unaffected.
- **No artifact was fetched.** Everything above was read from the **job logs**, which the
  new printer made sufficient.
