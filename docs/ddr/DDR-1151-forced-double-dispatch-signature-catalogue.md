# DDR-1151 — What does the double dispatch actually produce? A forced-mechanism signature catalogue for OPEN-2 and OPEN-1 route 1

**Status:** DESIGN (committed before the code, NON-NEGOTIABLE 5). **MEASURED — §7.**
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

## 7. Results (2026-09-26)

**Status becomes: MEASURED.** 42 local boots on three pinned binaries, one QEMU
at a time. Each kernel hash was re-checked after every run, and every capture
was checked non-empty before it was scanned. Every address below was resolved
against **its own** binary (§INV.18). Copies of those binaries are kept under
`build/gatelogs/dd/`. The catalogue is produced by `tools/ci/dd_analyze.sh`
(`DIR=` the captures, `ELF=` the binary that produced them).

| campaign | binary | boots | red | notes |
|---|---|---|---|---|
| mutant, spin 2000 | `ea27072ca1c776e4` | 24 (6 × smpuser / surfdestroy / blk-integrity / rqstress) | **24** | `dblclaim=0` throughout, as designed |
| mutant, spin 200 | `bc188eff8ec007a1` | 6 × surfdestroy | **6** | 1 boot got past `SYSFSTAT OK` |
| **control, KEEPCAS** | `26b30a1517fa28b8` | 12 (3 × each gate) | **2** | `dblclaim` 33–164 per boot: every forced duplicate that reached a second claimer was refused |

**One process note, recorded rather than hidden.** The first spin-200 start
overwrote spin-2000's `smoke-surfdestroy-1` capture. The script reused capture
names across campaigns. Its catalogue entry survives in `catalogue1.txt`. The
script now requires a per-campaign `DDOUT` directory.

### 7.1 What the forced mechanism produces (the reachable bin)

Counts are per boot. The main table covers the 29 surviving mutant captures;
the lost `surfdestroy-1` is taken from its catalogue entry and counted in the
`#UD` row only.

| signature | boots | resolved as | historical match |
|---|---|---|---|
| `[schedcheck]` `rq_on=0`, `disp−saves` = **+1, +2, +3** | 20 | every one of 25 fires: `r15=finish_task_switch+0xd`, `ret=schedule_locked+0x730` (the return address after `call local_irq_restore`), `rflags = rsp+0x30` | DDR-1115/1118/1120/1133/1134 consumed-frame family, **three witnesses exact** |
| `[apfreeze]` at `schedule_locked+0x6f2` | 15 | the `[schedcheck]` halt loop | DDR-1128/1129 |
| `[apfreeze]` at `isr_dispatch+0xfe7` | 9 | winner's terminal halt after a complete panic report | DDR-1099 / DDR-1134 lane 12 |
| `[apfreeze]` at `isr_dispatch+0x9b6` | 3 | **panic-loser `cli;hlt`**, right after `g_panic_loser_rip` is stored | **DDR-1019's shard-9 producer** |
| `[apfreeze]` at `spin_lock_contended+0x92` | 2 | vblk `compl_lock` spin | DDR-1121/1137 |
| `#DB` at `local_irq_restore+0x13`, `RFLAGS=0x106` (**TF**), `R15=finish_task_switch+0xd` | 1 | a `popfq` restoring a TF-set word from a spent frame | **DDR-1099's two-witness signature.** Different `popf` site, same class |
| `#PF err=0x11` (instruction fetch), RIP inside `g_percpu` / `g_inst` | 9 | executing data, i.e. a `ret` into a non-text address | — (new; stated without a match) |
| whole trap frame filled with `F000:FF53` (the real-mode interrupt vector table) | 1 (smpuser-5) | a `ret` through low memory | **DDR-1010's `pid=0xF000F053` shape**, without `gs FAIL` |
| `#UD` at RIP `0x2D` | 1 | a `ret` to a small integer | — |
| silent panic losers (`loser_vec=14`, RIPs in `g_percpu`/`g_rq`/`g_console_lock`) | 4 | the loser faulted fetching data | DDR-1019/1049 class |
| vblk `compl wait timeout`, `rqcpus>1` | 20 | downstream of a halted CPU (DDR-1115's chain) | DDR-1115/1119 |
| **whole-machine silent stop**: zero heartbeats, no panic, no `[apfreeze]`, last line `PRADYOS_FS_BUDGET_OK` | 4 (2 at each spin) | two `[schedcheck]` lines, then nothing | **OPEN-1 route 1's shape** (see §7.3) |

**The counter question DDR-1133 §10 left open is answered by measurement.**
One forced mechanism produced `disp−saves` of +1, +2 **and** +3, all at
`rq_on=0`. `rq_on=0` is the expected value, because the second picker already
popped the duplicate. So `disp = saves + 2` with `rq_on=0`, the combination
DDR-1131's pre-registration had no row for, **is** a double dispatch. And
`saves+1` does **not** exonerate a frame. DDR-1118's arithmetic identity cannot
discriminate this mechanism in either direction.

### 7.2 What it did NOT produce (not reachable at these settings)

- **U1** (`#GP` in `cap.c` `resolve`, `loser_vec=13`): **zero `#GP` in 30 mutant
  boots.** Not shown reachable. This is not proof that it is unreachable: 30 boots
  of a forced window say nothing about a rare branch. **U1 stays unattributed.**
- **Four of DDR-1133 §9.2's `ret` classes** (post-`context_switch`, `0x0`, `0x2`,
  and `r15` = `switch_wait_offcpu_sched`'s return): **not reproduced.** Every
  forced fire took the single consumed-frame class. The other classes stay
  unattributed.
- **U3** (single-CPU vblk timeout) is excluded by construction (§5).
- **U2**'s body is lost. What can be said is that every panic body the mechanism
  produced points at the switch path.

### 7.3 OPEN-1 route 1

- **Shape reproduced.** 4 of 30 mutant boots stopped the whole machine
  silently: no panic, no `[apfreeze]`, not one heartbeat. That is route 1's
  CI-only signature (a stop that prints nothing).
- **Location not reproduced.** Those stops were earlier in boot than route 1's
  recorded stopping point (between `SYSFSTAT OK` and `SYSREAD OK`). The one
  mutant boot that reached that point, spin-200 run 4, **passed through it**
  (`SYSFSTAT OK` → `SYSREAD OK` → `SURFDESTROY_OK`). The forced window fires
  early in boot, so where the machine stops is set by the forcing, not by
  route 1's workload.
- **Why a silent stop can happen at all is visible in the captures.** Unlike
  `[apfreeze]`, which needs a *live* BSP to notice a frozen AP, a double dispatch
  can land on the BSP itself. Then nothing is left to print.

**Verdict, stated at its real strength:** route 1 is **consistent with** the
fixed mechanism and is **not attributed** to it. The evidence still missing is
named in §8.

### 7.4 The control, and a pre-registered reading that did NOT hold

§5's last row predicted *"KEEPCAS shows `dblclaim>0` with no downstream
signature → the CAS alone suffices"*. **That reading is refuted.**

- **10 of 12 boots were clean** while refusing 33–164 double claims each. Zero
  `[schedcheck]`, zero panics, zero `#PF`/`#DB`/`#UD`, against 30 of 30 red in
  the mutant.
- **`blk-integrity-1` froze CPU 1 at `sched_exit+0x1b0`.** That address is the
  `hlt; jmp` after `sched_exit`'s `call schedule`, the trap for "an exited
  thread was resumed". The backtrace is `sys_exit+0xd9 ← syscall_dispatch+0x131
  ← syscall_entry+0x8a` (pid 44).
- **The mechanism is a stale token, not a concurrent one.** A duplicate pushed
  while the thread was READY outlives the thread's exit. It is later popped and
  claimed uncontested, and the ZOMBIE is resumed out of `schedule()`. The CAS
  has nothing to refuse, because nobody else holds the claim.
- **So DDR-1139's fix (a) is load-bearing on its own, and (c) is
  defence-in-depth against the concurrent case only.** All three fixes ship
  together, so the shipped kernel is unaffected. The finding is about the
  *reasons*.
- **This is also a new catalogue entry: `[apfreeze]` at `sched_exit`'s `hlt`.**
  If it ever appears on a shipped binary, a duplicate-token source DDR-1139 did
  not find is live.
- **`blk-integrity-3`** failed as `workers-late` with all four workers
  progressing (`prog=12,14,13,10`, `spawned=4/4`). By §INV.2 that is a
  scheduling-latency reason. The forced spin adds exactly that latency: 2,000
  pauses per forced duplicate, and 8× that at the two widened sites. It is
  recorded as an **instrument cost** and is **not** attributed to a double
  claim, because none was granted.

## 8. What would close each item, and what is still missing

| item | status after this DDR | the missing evidence |
|---|---|---|
| OPEN-2, DDR-1139 mechanism | confirmed (DDR-1139 §8); this DDR shows **seven historical signature families** are reachable from it | — |
| OPEN-2, U1 (`resolve` `#GP`) | **cannot be attributed with current evidence** | a recurrence on a **post-fix** binary (refutes the attribution), or a forced-mechanism `#GP` in `cap.c` (supports it) |
| OPEN-2, U2 (lane-12 panic) | its body is lost | none recoverable; closes only by not recurring |
| OPEN-2, U3 (single-CPU vblk timeout) | out of this mechanism's reach by construction | DDR-1148's `[vblkto]` line on its next occurrence |
| OPEN-1 route 1 | **consistent with, not attributed to** the fixed mechanism | a route-1 capture from a post-fix binary (would refute it), or one carrying a `[schedcheck]` line (would name it). The ongoing post-fix hunt is the instrument. |

**No fix is proposed** (NON-NEGOTIABLE 3): the mechanism these signatures
belong to is already fixed. **No rate is claimed.** The product binary is
**bit-identical** with the flags at 0.
