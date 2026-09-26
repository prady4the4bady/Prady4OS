# DDR-1151 — What does the double dispatch actually produce? A forced-mechanism signature catalogue for OPEN-2 and OPEN-1 route 1

**Status:** DESIGN (committed before the code, NON-NEGOTIABLE 5).
**Date:** 2026-09-26
**Asked by:** the operator on PR #17 (comment 5848314421, OWNER): *"put full attention on OPEN-1 route 1 and OPEN-2 until each either has a named mechanism and a real fix (mutation-tested) or you report back explicitly that it cannot be closed with current instrumentation and name exactly what evidence is still missing."*

## 1. Where the two issues stand

**OPEN-2.** DDR-1139 named one mechanism and fixed it: `schedule_locked` re-queued a `prev` that `sched_unblock` had already queued, which made two live runqueue tokens and let two CPUs claim one thread. The fix was confirmed against its pre-registered criterion: 0 signals in 2,200 fixed-kernel hunt boots, against 4 in 20 lanes before the fix.

DDR-1139 §6 did **not** claim every OPEN-2 producer. Three signatures remain unattributed:

- **(U1)** DDR-1133 §6: a silent panic with `loser_vec=13` at `resolve+0x61` (`cap.c:42`). A #GP on a plain load means a non-canonical address.
- **(U2)** DDR-1134 §5: lane 12, a completed panic report whose body the old hunt printer dropped. Its cause was never read.
- **(U3)** DDR-1148: a single-CPU (`rqcpus=1`) vblk `compl wait timeout` on `smoke-kill`. The CPU was alive.

**OPEN-1 route 1.** This is a CI-only stop after `SYSFSTAT OK` in `smoke-surfdestroy`. The second occurrence was on `smoke-msixap` and panicked with no body. No occurrence has been recorded since DDR-1009, and it is stated open (DDR-1124).

## 2. The question this answers, and the one it does not

The question is which signatures a double dispatch produces when it happens. Every unattributed signature above was observed on a pre-fix kernel, where the double dispatch was live.

- **If the mechanism produces a signature, the signature is inside the fixed mechanism's reach.** Its absence in 2,200 post-fix boots then has an explanation that does not require a second defect.
- **If the mechanism never produces it, even when forced, it is a separate defect.** It must be named separately, as DDR-1139 §6 already promised.

What this does **not** do is prove that any one historical occurrence was the double dispatch. A matching shape is not a mechanism (DDR-1056), and this DDR does not claim otherwise. It moves each unattributed signature into one of two bins: *reachable from the named, fixed mechanism* or *not reachable from it*.

## 3. The instrument: `OPEN2_FORCE_DD=<spins>`

This follows the `OPEN2_FORCE_RECYCLE` precedent (DDR-1097). It is a build flag, default `0`, and with `0` the product binary is **bit-identical**. With `N > 0`:

1. **The pre-fix duplicate push is restored.** A `prev` that entered `schedule_locked` already READY and is not idle is `rq_push`ed again. This is DDR-1139 §2's pre-fix line verbatim.
2. **The window is widened.** If that duplicate push actually created a second token (`rq_on` was 0 before it), this CPU spins `N` `pause` iterations before switching away. Its `prev->on_cpu` stays set for that time, so another CPU has room to pop the duplicate and wait on `on_cpu` beside the holder of the first token. The spin is bounded and `N` stays below `switch_wait_offcpu_sched`'s 4096 bail, so the waiters are still waiting when `on_cpu` is released.
3. **The exclusive claim (DDR-1139 (c)) is removed** in the mutant, so a double claim becomes a double run, as it did before the fix.
4. **The self-pick keep-running (DDR-1139 (b)) is also removed**, restoring the pre-fix wait-on-self.

This is **the pre-fix code plus a wider window**. It adds no new code path, so what it produces is what the pre-fix kernel could produce, only more often. Arm 3 is what makes a double claim reach execution. A variant with (c) kept, `OPEN2_FORCE_DD_KEEPCAS=1`, is the control. It should show `dblclaim>0` and **no** downstream signatures, which would demonstrate that the defence-in-depth CAS alone is sufficient.

## 4. The runs

These are local runs, one QEMU at a time (NON-NEGOTIABLE 12). The kernel hash is pinned and re-checked after every run (DDR-1060 §9). `KEEP_SERIAL=1` keeps each capture, and every capture is asserted non-empty before it is scanned (DDR-1023).

| gate | why | N runs |
|---|---|---|
| smoke-smpuser | where DDR-1139 §3 measured the duplicate push (`tkdup=1`) | 10 |
| smoke-blk-integrity | block path: U3's shape, and DDR-1121/1137's vblk spin | 10 |
| smoke-surfdestroy | **OPEN-1 route 1's gate** | 10 |
| smoke-rqstress | heaviest create/exit churn | 10 |

For each capture, the catalogue records every `[schedcheck]` (its `disp−saves` and `rq_on`), every `[apfreeze]` RIP resolved against the mutant's own binary (§INV.18), every panic's vector and RIP, `panics_silent`/`panic_stage`, and whether the boot **stopped silently**, which is route 1's shape.

## 5. Readings, pre-registered before the data

| outcome | reading |
|---|---|
| The mutant produces a #GP silent panic in `cap.c`/`resolve` or any `loser_vec=13` on a plain load | U1 is **reachable** from the fixed mechanism |
| A panic report with a body appears | the body is read, and U2's class is decided by where that body points. U2's own body is lost for good, so U2 itself stays unread |
| `smoke-surfdestroy` stops after `SYSFSTAT OK` with no panic, or with a bodiless panic | route 1 is **reachable** from the fixed mechanism |
| A vblk `compl wait timeout` with a live CPU appears **at `rqcpus>1`** | the shape is reachable. **U3 itself was single-CPU, where a double claim needs three CPUs, so U3 stays unattributed by construction** |
| The mutant produces zero double claims (`[schedcheck]` never fires, nothing breaks) | **the forcing failed** and nothing is concluded. This is a null on design, the DDR-1002 class |
| The KEEPCAS control shows `dblclaim>0` with no downstream signature | the CAS alone suffices, as measured, not argued |

## 6. Not claimed

- No new fix. The mechanism is already fixed. What this changes is the **attribution** of the leftovers.
- No rate. Forced rates say nothing about natural rates.
- U3 is excluded by construction, as stated above. It needs DDR-1148's instrument to fire again.
- The hunt on the current tip (`dfb1179`, 20×55, dispatched 2026-09-26) is a separate measurement. It is pooled with nothing, because it is a different binary from DDR-1139 §8's.
