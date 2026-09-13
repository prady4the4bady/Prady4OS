# DDR-1120 — THE THIRD `[schedcheck]` FIRE: THE FRAME IS CONSUMED (THREE EXACT WITNESSES) AND `disp − saves = 1` REFUTES THE DOUBLE-RESUME MECHANISM DDR-1118 PROPOSED

**Assessment of a captured artefact. NO FIX, NO MECHANISM NAMED, OPEN-2 DOES NOT
CLOSE.** No code change, no gate, `kernel.bin` NOT rebuilt.

**The instrument DDR-1118 added to test its own hypothesis refuted it on the
first capture that could.** That is the finding, and it is worth more than a
confirmation would have been.

---

## §0 — Artefact and provenance

CI **34766468421**, **shard 5 `smoke-smpuser`**, head **`5f3ac9e`** (DDR-1118 —
the kernel change). The shard's own post-gate step printed **`kernel.bin: OK`**,
so it ran the binary the build job published; the local pin is
**`6af029b001e6e6db`** at 1,319,306 B, and every address below is resolved
against that binary (§INV.18). A **second** shard on the same head also failed
at a different site — §6.

```
[schedcheck] next->rsp invalid tid=11 pid=0 rsp=0x0000000007C2A558 base=0x0000000007C28000
             rflags=0x0000000007C2A588 r15=0xFFFFFFFF8001613D ret=0xFFFFFFFF8001687F
             rq_on=0 disp=40669 saves=40668 halting.
```

**Clauses 2 and 3 passed**, measured not read off the line: `rsp` is 8-aligned,
`rsp − base = 0x2558 = 9560`, the 64-byte frame fits, and `rsp` sits **6,824 B
below the top** of a 16 KiB stack. So the pointer is not garbage in the wild
sense; only the frame's **content** is wrong. Third time that sentence has been
the correct one.

**The `[apfreeze]` is downstream of this halt, and that is now checkable rather
than inferred.** `[apfreeze] cpu=2 ticks=164 rip=0xFFFFFFFF80016841 if=0 pid=11`
— and `0xFFFFFFFF80016841` disassembles in this binary to the `jmp` of the
`hlt; jmp` pair at **`schedule_locked+0x670/0x671`**, i.e. **the halt loop at the
end of the `[schedcheck]` block**, entered only by falling out of `kline_emit` at
`0x1683b`. Single-entry, verified: the fall-through at `0x1684b` is the
`prev->switches_away++` path. The four `[apfreeze]` shots all report the same
frozen RIP and the same `ticks=164`, and every `[vblk] compl wait timeout`
afterwards reads `dest_cpu=2 dest_abs=164` against a BSP climbing to 5254 —
**DDR-1115's causal chain reproduced on a third artefact: the block failures are
downstream of the halt.**

---

## §1 — Three exact witnesses, and the chain runs one call further than DDR-1118 traced

Measured in **this** binary, never carried (§INV.18 — and DDR-1119 is the
reason the word "never" is in that sentence):

```
ffffffff8001686c:  callq <context_switch>          -> returns to 0x16871
ffffffff80016871:  callq <finish_task_switch>      -> pushes 0x16876
ffffffff80016876:  movq  -0x8(%rbp), %rdi
ffffffff8001687a:  callq <local_irq_restore>       -> pushes 0x1687f

ffffffff80016130 <finish_task_switch>:
  80016130:  pushq %rbp
  80016131:  movq  %rsp, %rbp
  80016134:  subq  $0x20, %rsp
  80016138:  callq <this_cpu>                      -> returns to 0x1613d   (= +0xd)
```

Let **S** be the value `context_switch` stored. Resuming from S:

| step | rsp | writes |
|---|---|---|
| 7 pops + `ret` | S+0x40 | jumps to `0x16871` |
| `call finish_task_switch` | S+0x38 | `[S+0x38] = 0x16876` |
| `push rbp` | S+0x30 | `rbp = S+0x30` |
| `sub $0x20` | S+0x10 | |
| `call this_cpu` | S+0x08 | **`[S+0x08] = 0x1613d`** |
| `this_cpu`'s `push rbp` | S+0x00 | **`[S+0x00] = S+0x30`** |
| …returns; `finish_task_switch` returns to `0x16876`; rsp back to S+0x40 | | |
| `call local_irq_restore` | S+0x38 | **`[S+0x38] = 0x1687f`** |

Against the capture:

| slot | predicted | observed | ✓ |
|---|---|---|---|
| `[S+0x00]` | `S+0x30` = `0x07C2A588` | `rflags=0x07C2A588` | ✓ |
| `[S+0x08]` | `0xFFFFFFFF8001613D` | `r15=0xFFFFFFFF8001613D` | ✓ |
| `[S+0x38]` | `0xFFFFFFFF8001687F` | `ret=0xFFFFFFFF8001687F` | ✓ |

**Three slots, three exact matches, no free parameters.** Stronger than fire 2,
which had two witnesses and `ret=0`; and the third witness **extends the chain
past the point DDR-1118 traced** — it is not the `finish_task_switch` return
address (`0x16876`) but the `local_irq_restore` one (`0x1687f`), which is what
sits at S+0x38 **after `finish_task_switch` has already returned**. So the thread
did not merely begin the switch-in: it **completed** `finish_task_switch` and ran
on into the irq-restore that ends `schedule_locked`.

**And the numerals moved again while the offsets did not.** `finish_task_switch`
is at `0x80016130` here against `0x80016120` in fires 1 and 2 — a shift of
0x10 — while **`+0xd` is the offset in all three**. §INV.18 vindicated a third
time, on a third binary.

---

## §2 — THE FINDING: the identity holds, so the thread was **not** dispatched twice

DDR-1118 §6 stated the discriminator in advance and stated it correctly:

> `disp == saves + 1` on EVERY healthy switch, and `disp == saves + 2` is a
> thread switched in twice with no save between — the finding as a NUMBER rather
> than an argument about stack litter.

**Observed: `disp=40669 saves=40668`. Difference = 1. The healthy identity.**

Confirmed against the disassembly rather than taken from the design text: the
check block reads both counters off `-0x28(%rbp)` (**`next`**) at offsets
`0x2848` and `0x2850`, while the fall-through at `0x1684b` increments
`0x2850` on `-0x10(%rbp)` (**`prev`**). So the line prints `next`'s dispatches
and `next`'s switches-away, `next->dispatches++` having already run under the
claim. `disp = saves + 1` is exactly "dispatched once more than it has switched
away", i.e. **being dispatched right now, normally**.

`rq_on=0` says the same thing from the other side: the thread was **not** also
sitting in a runqueue while this CPU resumed it. That was DDR-1118's named
*precondition* for the double resume, and it is **absent**.

**So the mechanism DDR-1118 proposed — "the scheduler is attempting to resume it
a SECOND TIME from the same spent value" — is refuted on this artefact.** The
frame is spent; the thread was not resumed twice.

This is the instrument working exactly as designed. DDR-1118 built `disp`/`saves`
because stack litter is an *argument* and an arithmetic identity is a *number*,
and it built `rq_on` because a double resume needs the thread to be queued while
running. Both fields answered, and both answered against the hypothesis that
motivated them.

---

## §3 — What the identity does **not** rule out, stated because the flattering reading is one sentence away

`disp − saves = 1` refutes **one specific** mechanism: the same TCB dispatched
twice with no intervening save. It does **not** rule out:

**(a) A stale `->rsp`.** If the thread's most recent switch-away did not write
`->rsp` — or wrote it somewhere else — the stored value is from an earlier
epoch, and the counters stay balanced because `switches_away++` and the `->rsp`
store are different operations. This is the reading the three witnesses most
directly support: a frame that a *completed* resume left behind.

**(b) A recycled / reissued TCB — DDR-1096 §3's hypothesis, and the identity is
blind to it.** `kmalloc` does not zero (NON-NEGOTIABLE 10), so a reissued TCB
carries its previous owner's counters. A thread that has switched away and is
not running satisfies `old_disp == old_saves`; one `dispatches++` under the claim
then yields `disp = saves + 1` — **the healthy identity, on a recycled object.**
Stated plainly because it would be easy to read `disp − saves = 1` as "the TCB is
sound", and it is not that. `OPEN2_HUNT`'s `tid` re-check (DDR-1097) is the
instrument aimed at exactly this, and it has now — as of today — finally been
made startable (DDR-1117, landed on the default branch, run #1 dispatched).

**(c) Anything about `sched.c:202-207`'s rq-2 exclusion**, which DDR-1119 named
as where a third fire would point. `rq_on=0` is evidence *against* the
queued-while-running shape, but the `switch_wait_offcpu_sched` `on_cpu`
handshake and its 4096-spin bail are untouched by this capture.

---

## §4 — The three fires are not the same thread, and that is new

| | fire 1 (DDR-1115) | fire 2 (DDR-1118) | **fire 3 (here)** |
|---|---|---|---|
| tip / binary | `a5d876e` / `95493b96…` | `be0b2ad` / `f8574d7d…` | `5f3ac9e` / `6af029b0…` |
| gate | `smoke-swapgs` (shard 6) | `smoke-nethammer` (shard 3) | `smoke-smpuser` (shard 5) |
| tid / pid | 22 / — | 22 / 22 (**ring 3**) | **11 / 0 (kernel thread)** |
| depth below ktop | 1472 B | 1520 B | **6824 B** |
| `[S+0x00] − rsp` | **0x30** | **0x30** | **0x30** |
| witnesses | 1 | 2 | **3** |
| `rq_on` / `disp−saves` | — | — | **0 / 1** |
| frozen CPU | 1 | 1 | **2** |

DDR-1119 recorded tid 22 twice as a **weak** narrowing (`next_tid++` is assigned
in boot order, so the same probe gets the same tid on every boot of the same
sequence) and said so. **Fire 3 breaks it**: a different tid, a *kernel* thread
rather than a ring-3 process, at four times the stack depth, on a different CPU
index. So the family is not "tid 22 is special", and the one invariant that has
survived all three is the **`[S+0x00] = rsp + 0x30` relationship** — which is
structural, not incidental, because it is what the resume chain writes there.

`disp=40669` is itself worth recording: this is a long-lived, extremely busy
kernel thread, not a short-lived probe. Whatever selects a spent frame is not
confined to thread creation or teardown.

---

## §5 — A splice, recorded not chased

The same capture carries

```
[vblk] compl wait timeout unit=1 dest_cpu=[yieldstall] site=mnt_lock spins=38541 ticks=2 dest_dticks=0500 pid= dest_abs=16455 cpu= bsp_abs=3
754 dest_present=1 ticks[754,733,164,730] on_cpu=0 lba=2050
```

Two ring-0 lines interleaved character-wise — the **DDR-1055 console splice
class**, on lines built from several `kputs` calls rather than one `kline` emit.
Not this DDR's subject and **not attributed to anything here**; recorded so the
next reader does not try to parse `dest_cpu=[yieldstall]` as a field.

Also in the capture and **not** a defect: `[yieldstall] site=mnt_lock` fires
repeatedly on **cpu 0** and then prints `RESOLVED` twice (`spins=254939
ticks=4000`, `spins=1080585 ticks=4500`). DDR-994's detector doing its job on
the unbounded `mnt_lock` wait, and **resolving** — a different thread from the
frozen one.

---

## §6 — The second red on the same head is a different site

**shard 7 `smoke-smpsched`**, same head `5f3ac9e`, `kernel.bin: OK`:

```
[apfreeze] cpu=2 ticks=162 rip=0xFFFFFFFF80049A52 if=0 pid=22
           bt=0xFFFFFFFF80021405,0xFFFFFFFF8002139C,0xFFFFFFFF8002149E,0xFFFFFFFF80021094
```

Resolved against the same binary:

```
spin_lock_contended+0x92  <- spin_lock+0x25  <- spin_lock_irqsave+0x1c
                          <- submit+0x2e     <- vblk_read+0x34
```

**A lock wait, not a halt loop** — a CPU spinning on the virtio-blk submit lock
with `IF` clear. This is **not** a `[schedcheck]` fire and must not be pooled
with one: the scan reported `[apfreeze]` as its only match on that shard.

It is, however, precisely the case **DDR-1060** built `waiters=` for, and the
lock dump is ordered after the `[apfreeze]` line, so the next occurrence should
carry it. **NOT attributed and NOT explained here.** Two shards freezing on
`cpu=2` at ticks 162/164 on one binary is recorded as an observation; **no rate
is claimed and no common cause is asserted.**

---

## §7 — NOT CLAIMED

- **NO FIX. NO mechanism named. OPEN-2 does not close** (NON-NEGOTIABLE 3).
  No open issue moves — OPEN-1/2/12/13 untouched.
- **NO code change**, no gate run, `kernel.bin` NOT rebuilt, so the size/headroom
  pair and `ci-docstate-check` are unaffected. `GLOBAL_FORBIDDEN` **77**, **179**
  gates, 79 probe ELFs — all unchanged. No new sentinel, no new clause: **the set
  of frames that fire is unchanged.**
- **DDR-1118 IS NOT WITHDRAWN.** Its consumed-frame *reading* is confirmed here
  by three exact witnesses rather than two, and its instrument is what produced
  this result. What is refuted is the **mechanism** it proposed on top of that
  reading — and it is refuted by the field DDR-1118 itself added for the purpose,
  which is the instrument succeeding, not failing. DDR-1119 likewise stands.
- **NOT exonerated in advance** (DDR-1042): `5f3ac9e` edits this exact path and
  adds an increment to the hottest function in the kernel. Two shards red on that
  head is consistent with an unlucky draw on a known rare intermittent **and**
  with a perturbation, and this capture does not separate them. No claim either
  way.
- **NO RATE.** Three fires ever, one carrying the new fields. No campaign was run
  to manufacture a fourth.
- The `[apfreeze]` on shard 7 is **not** explained, and the `mnt_lock`
  `[yieldstall]` lines are **not** attributed to the freeze.
- **What a fourth fire should be read for**, now that the double-resume shape is
  off the table for at least one capture: whether `disp − saves` is *ever* 2
  (which would restore it for a different fire), and whether `tid` is stable
  across the `OPEN2_HUNT` pause — the recycled-TCB reading of §3(b) is the one
  this artefact leaves standing and the one the now-startable hunt is aimed at.
