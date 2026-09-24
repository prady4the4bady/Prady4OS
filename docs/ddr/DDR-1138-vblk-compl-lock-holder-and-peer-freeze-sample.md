# DDR-1138 — Name the HOLDER of a stuck vblk `compl_lock`, and sample every frozen CPU, not only the first

**Date:** 2026-09-24
**Status:** DESIGN, committed before any code (§NON-NEGOTIABLE 5). Instrument change
only. **NO FIX, NO MECHANISM NAMED. OPEN-2 DOES NOT CLOSE.**
**Asked for by:** the operator on PR #17 (comment 5808760525, verified at the source as
`author_association: OWNER`): *"try to capture the lock holder this time, not just the
waiter."*

---

## §1 — WHAT DDR-1137 LEFT OPEN, AND WHY NO EXISTING INSTRUMENT CAN CLOSE IT

DDR-1137 §3.2 found that at the vblk freeze site the backtrace and the lock table
**agree** for the first time: the live spin waiter is on `g_inst+0x828`, which is unit 1's
`compl_lock`. It could not say **who holds that lock**, for two structural reasons.

1. **Nothing in the kernel records a spinlock's owner.** `spinlock_t` is one byte
   (`spinlock.h:12`). `lock_stat` (DDR-1047/1060) sees only the **slow path**, so it
   knows who is WAITING and never who is HOLDING. DDR-1047 refused an owner store on the
   global fast path for a measured reason (the hottest primitive in the kernel, on
   OPEN-2's own timing path), and that refusal stands.
2. **`[apfreeze]` samples one CPU.** `s_victim` latches the lowest-indexed frozen CPU and
   `continue`s every other one (DDR-1122). Both DDR-1137 vblk captures had **two** frozen
   CPUs, and **the unlatched CPU froze first** in both. That CPU's RIP has never been
   captured at this site.

DDR-1137 §3.2 also named the reading that makes (1) matter more than (2). Lane 9 read
`waiters=2` with two frozen CPUs. If both frozen CPUs are waiters, **the holder is not a
frozen CPU**, and sampling more frozen CPUs would never find it.

## §2 — THE DESIGN: TWO HALVES, EACH ANSWERING A DIFFERENT QUESTION

### §2.1 Holder record on the vblk `compl_lock` only (answers "who holds it")

- `struct vblk` gains a small owner record: `own_cpu`, `own_tid`, `own_site`, `own_tick`.
- It is written **immediately after every acquisition** of `compl_lock` and cleared
  **immediately before every release**. The acquisitions are:
  - `complete()` (the completion ISR);
  - `submit()` entry;
  - the two returns from `sched_block_timeout()` inside `submit()`. That function drops
    and re-takes the lock internally, so the record is cleared before the call and set
    again after it returns. The record is therefore never stale while the thread sleeps.
- The site tag names which acquisition (1 = complete, 2 = submit entry, 3 = after a
  slot wait, 4 = after a completion wait).
- CPU identity is `this_cpu()->cpu_idx`, which **both functions already use today**
  (`complete()` reads `this_cpu()` for DDR-714C3's `compl_ap`; `submit()` reads it for the
  DDR-976 timeout line). No new GS dependency is added on this path.
- **Printed only on the freeze path.** After the existing once-per-boot
  `lock_stat_dump()` in the `[apfreeze]` relay, a new `virtio_blk_dump_owners()` prints one
  line per unit, as `[vblkown] unit=… locked=… own_cpu=… own_tid=… site=… since=…`.
  - The lock byte and the record are read **without taking the lock**. The reader is a
    diagnostic on a machine that has already frozen, and taking the lock is exactly what
    would hang it.
  - `locked` is the raw byte, printed beside the record, so a reader can see a disagreement
    between the two instead of the dump hiding one.

**Why this and not an owner field on every spinlock.** Every clause of DDR-1047's refusal
fails to reach it:
- it is not the hottest primitive; it is one driver's lock;
- its cost is a few plain stores on a path that already does MMIO (`virtio_pci_notify`)
  and already sleeps;
- it is aimed at the **one** site where three captures on one binary have now frozen
  (DDR-1136 §3.4, DDR-1137 §3.2 ×2), rather than designed off a single occurrence.

**What the answer can look like, stated in advance so it is read against a fixed table:**

| `locked` | record | reading |
|---|---|---|
| 1 | `own_cpu` = a frozen CPU | holder is frozen; compare with that CPU's peer sample (§2.2) |
| 1 | `own_cpu` = a ticking CPU | holder is running; with IRQs off it cannot tick, so this is a contradiction to report, not explain |
| 1 | empty (`own_cpu=none`) | the lock is held with no recorded owner: an acquisition this record does not cover, or a release that skipped the clear |
| 0 | any | nobody holds it at the instant of the dump; the waiter's spin should end |

None of these rows names a mechanism. Each one narrows which kind of mechanism to look for.

### §2.2 One NMI sample for every OTHER frozen CPU (answers "what is the other CPU doing")

- `s_victim` and its 4-shot budget are **kept exactly as they are**. Four shots on one CPU
  are what make a pinned RIP mean "spinning" (DDR-981, DDR-1122).
- In addition, each other present non-BSP CPU that is frozen for a full window gets **one**
  NMI, once per boot. Its dump is relayed by the same arm and prints the same
  `[apfreeze]` line, with **`peer=1`**. Victim lines print `peer=0`.
- The budget is at most one extra NMI per CPU per boot (≤ `PERCPU_MAX`), on the freeze
  path only.
- The victim's cadence (one shot per heartbeat) is unchanged. After a victim shot the scan
  continues so that peers can be armed in the same heartbeat.

## §3 — PROOF, WITH THE VACUITY CHECK DONE FIRST

The triggering condition cannot be manufactured in the product (DDR-1105 §8), so each half
is proved with a **forced mutant on a recorded hash**. Each mutant changes one thing
(DDR-1042).

- **§2.1.**
  - The vacuous arm is "force a dump and see `[vblkown]` print". A dump taken while
    nothing holds the lock prints `locked=0 own_cpu=none`, whether or not the recording
    works.
  - So the forced dump is taken **from inside `complete()` while it holds the lock**. Only
    a working record prints `locked=1 own_cpu=<that CPU> site=1`.
  - **M1 (the record is never written) must print `locked=1 own_cpu=none`** from the
    identical trigger. That is the plausible wrong implementation, and a check that only
    looks for the line would pass it.
- **§2.2.**
  - The vacuous arm is "force a freeze and see an `[apfreeze]` line". The victim line
    prints on the old code too.
  - So the frozen predicate is forced for **several** CPUs on a healthy `-smp 4` boot.
    The new code must print `peer=1` lines, one per other forced CPU, each with its **own**
    `cpu=`.
  - **The control is the pre-change tree with the identical forcing**, which prints the
    victim alone (the DDR-1066 form: the pre-fix tree is the control).
- **The negative half, which matters more, since this adds stores to the block path:**
  - `smoke-shell` 5/5 plus `smoke-smp`, `smoke-smppreempt`, `smoke-rqstress`,
    `smoke-blk-integrity` and `smoke-blkmq` must all be `rc=0`.
  - At least one retained capture must be non-empty and carry **zero** `[vblkown]` and
    zero `peer=` lines. A healthy boot must print none of this.

## §4 — NOT CLAIMED

- **No fix and no mechanism. OPEN-2 does not close.** This changes what the **next**
  vblk-site freeze can say, not anything about the three already captured.
- **No defect is alleged** in `virtio_blk.c`, `spin_lock_contended`, `lock_stat.c`,
  `sched_block_timeout` or `ap_freeze_probe`. `s_victim` stays correct for what it was
  built to do.
- **Not exonerated in advance** (DDR-1042). This adds plain stores inside the very
  critical sections where the freeze is observed. If the vblk freeze stops appearing after
  this lands, **that is a candidate perturbation, not evidence of a fix**. The hunt
  dispatched on 2026-09-24 (run 35963517515) is pinned to the **pre-change** SHA
  `d7257ad` by full 40-character ref, so its data stays on the old binary.
- **DDR-1047's refusal is not revisited.** No owner store is added to `spin_lock`.
- No new gate (179), no new sentinel (`GLOBAL_FORBIDDEN` 77).
  - `[vblkown]` prints only after `[apfreeze]`, which is already forbidden, so it needs
    no entry of its own.
  - It falls inside the 40 lines of trailing context that DDR-1135's hunt printer shows
    after every match.
