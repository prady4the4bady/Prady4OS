# DDR-1122 — THE SHARD-7 CAPTURE HAS **TWO** FROZEN CPUs, AND THE `[apfreeze]` DETECTOR CAN ONLY EVER REPORT ONE

**Assessment + correction of DDR-1121 §4's elimination step. Docs-only: no code
change, no gate, `kernel.bin` NOT rebuilt. NO FIX, NO MECHANISM NAMED, OPEN-2
DOES NOT CLOSE.**

Date: 2026-09-19

---

## §0 — Provenance, §INV.18 satisfied without a rebuild

Same artefact as DDR-1120 and DDR-1121: **CI run 34766468421**, on `5f3ac9e`.
Fetched with `failed_only=true`, which returns **both** failed jobs of that run
in one file — and that is how this was found, because the two captures sit in
one document and can be read against each other:

| | gate | `[apfreeze]` | analysed by |
|---|---|---|---|
| block A | shard 7 `smoke-smpsched` | `cpu=2 ticks=162 rip=0x…49A52 pid=22` | DDR-1121 |
| block B | shard 5 `smoke-smpuser` | `cpu=2 ticks=164 rip=0x…16841 pid=11` | DDR-1120 |

Every address below is resolved against **the exact binary that produced the
capture**, checked rather than assumed: `build/kernel.elf` re-derives under
`llvm-objcopy -O binary` to `6af029b001e6e6db`, byte-for-byte the on-disk
`kernel.bin` at **1,319,306 B**, and `git diff --name-only 5f3ac9e HEAD` over
`Makefile` and every `.c/.h/.asm/.S/.ld` returns **ZERO FILES**.

Resolution is done in **python3**, never `awk`: `strtonum()` is a gawk extension
and this host's `/usr/bin/awk` is mawk. DDR-1079 fixed exactly that in
`tools/ci/sym_at.sh`; DDR-1121 recorded that the trap had then cost time a
second time. It did not cost time a third.

---

## §1 — THE FINDING: `ticks[684,663,162,160]`, FOURTEEN TIMES

DDR-976 added the full per-CPU tick census to the `[vblk] compl wait timeout`
line, and said in its own comment at `virtio_blk.c:314` exactly what it is for:

> *"`only unit 2 times out` is not by itself evidence that CPU 3 is the sick
> one … **This line settles which: one frozen counter among moving ones, or all
> APs frozen.**"*

It settles it. In the shard-7 capture the census appears **fourteen times**,
from `t=500` to `t=7000`, and every one of them reads:

```
ticks[684,663,162,160]      (t=500)
ticks[1184,1163,162,160]
…
ticks[7184,7163,162,160]    (t=7000)
```

CPU 0 and CPU 1 climb from 684/663 to 7184/7163. **CPU 2 is pinned at 162 and
CPU 3 is pinned at 160, for the entire ~65 s of the boot.**

**TWO CPUs ARE FROZEN, NOT ONE.** And the two instruments name *different* ones:

- every `[vblk] compl wait timeout` reads **`dest_cpu=3 dest_dticks=0
  dest_abs=160`** — the block layer's victim is **CPU 3**;
- every `[apfreeze]` line reads **`cpu=2`**.

**CPU 3 STOPPED FIRST**: 160 against 162, and `dest_cpu=3` is already in the
first timeout at `t=500`.

### §1.1 — The contrast with the sibling capture is what makes this sharp

The shard-5 capture, same binary, same run, carries the same census and reads
the other way:

```
ticks[754,733,164,730]      (t=754)
…
ticks[5254,5233,164,5230]   (t=5254)
```

**Exactly one frozen CPU (index 2)**, `dest_cpu=2`, `[apfreeze] cpu=2` — all
three instruments agreeing. So the difference is not an artefact of how the
census is printed; it is a property of the two boots, and only one of them has
it.

---

## §2 — THE DETECTOR REPORTS ONE CPU **BY CONSTRUCTION**, AND HAS ALWAYS DONE SO

`ap_freeze_probe()` (`kernel/idt.c`) latches a single victim:

```c
static int s_victim = -1;        /* the one CPU we sample, once chosen */
…
if (s_victim >= 0 && s_victim != (int)i)
    continue;
s_victim = (int)i;
```

and its own comment states the reason, which is a **good** reason:

> *"stay on the FIRST frozen CPU (`s_victim`): a walking RIP across shots means
> the CPU is running and merely masked, a pinned RIP means it is spinning. That
> is the question a single shot cannot answer."*

Spending all four shots on one CPU is what makes `shot=1..4` with an identical
RIP mean *spinning* rather than *slow*. Nothing here says that choice was
wrong.

**What is wrong is a reading nobody stated and several DDRs used: that ONE
`[apfreeze]` LINE MEANS ONE FROZEN CPU.** It does not, and it never did. The
line is a **latch**, not a census. Every later frozen CPU is `continue`d, so the
instrument is structurally incapable of reporting a second one — and the loop
scans `i` ascending, so it picks the **lowest-indexed** frozen CPU in the
window, which in this capture is **CPU 2, the one that froze *second***.

DDR-1115 §2 wrote, carefully and with the caveat: *"'CPU 1' is an inference from
three facts (**exactly one CPU frozen**, its ticks equal to `dest_abs`, the halt
at a consistent point)"*. The first of those three facts was never measured — it
was read off the number of `[apfreeze]` lines. **In DDR-1115's and DDR-1119's
own captures the census does show one frozen CPU** (`ticks[886,168,863,862]`
and `ticks[865,162,910,783]`), so those two inferences happen to be sound; they
are sound **by luck of the capture**, not because the instrument establishes it.

---

## §3 — WHAT THIS DOES TO DDR-1121, AND WHAT SURVIVES

DDR-1121's four propositions are each still individually correct and each is
still measured. What fails is the step that joins them:

> *"BY ELIMINATION THE +1 ON `g_rq[1].lock` IS THE FROZEN CPU'S."*

and

> *"'the +1 is an unrelated transient waiter on `g_rq[1]`' … DOES NOT RESCUE THE
> PICTURE, because **the frozen CPU's own +1 would then have to appear
> somewhere** and it appears nowhere."*

Both sentences say **the** frozen CPU. There are two. The elimination has a
different shape once that is admitted:

- only **CPU 2** is known to be inside `spin_lock_contended` (its RIP,
  `spin_lock_contended+0x92`, is the `jmp` closing the spin loop, after
  `waiters++` at `+0x5B` and before `waiters--` at `+0xCB`);
- **CPU 3's RIP was never captured at all** — the detector spent all four shots
  on CPU 2 — so nothing says whether CPU 3 is spinning, halted, or masked;
- so **one** live `+1` is exactly what the table should hold if CPU 3 is not in
  a spin, and **two** if it is. The table holds one.

**THE CONTRADICTION IS NARROWED, NOT RESOLVED, AND IT IS NOW A DIFFERENT
SENTENCE.** It is no longer *"the frozen CPU's +1 is missing"*. It is:

> **the one live `+1` sits on a runqueue lock, and the one CPU whose backtrace
> was captured was blocked on a `compl_lock`.**

That is still a disagreement between two instruction-exact witnesses and this
DDR does not close it. It is a smaller claim than DDR-1121's, and it is the
honest one.

### §3.1 — A reading this opens and explicitly does NOT assert

CPU 3 froze **first** (160 < 162); the block layer names CPU 3; CPU 2 is
blocked acquiring a `compl_lock` inside `submit ← vblk_read`. The shape
*"CPU 3 wedged first and CPU 2 then piled up behind the block path"* fits every
number in the capture.

**IT IS NOT CLAIMED.** A matching shape is not a mechanism (DDR-1056), the
table still does not show a waiter on any `compl_lock`, and CPU 3's RIP —
the one datum that would settle it — **was never captured, because the detector
cannot capture a second CPU.**

---

## §4 — THE DDR-1120 CAPTURE ALSO CARRIES A LOCK DUMP, AND NOBODY HAS READ IT

DDR-1120 §6 closed with *"the lock dump is ordered after the `[apfreeze]` line
so the next occurrence should carry it"*. DDR-1121 §1 corrected that by finding
the dump already in the **shard-7** capture. **It is in the shard-5 capture
too** — in the same job-log fetch, 150 lines further down — and no DDR has
recorded it. `grep -rn '8016604C' docs/` returns nothing; the only occurrence of
`yield lock=` anywhere in `docs/` is DDR-1060's own format documentation.

It reads (15 entries, `overflow=0`, so complete):

```
PRADYOS_LOCKSTAT overflow=0
… 13 spin locks, EVERY ONE waiters=0 …
PRADYOS_LOCKSTAT yield lock=0xFFFFFFFF8016606C waits=5 waiters=0   (g_mounts+0x3C)
PRADYOS_LOCKSTAT yield lock=0xFFFFFFFF8016604C waits=1 waiters=3   (g_mounts+0x1C)
```

**Two things, and the first is the valuable one.**

**(a) EVERY SPIN LOCK READS `waiters=0`, AND THAT IS THE CORRECT ANSWER.**
DDR-1120 established that capture's freeze RIP is `schedule_locked+0x670/0x671`,
the `hlt; jmp` pair at the end of the `[schedcheck]` block — a **halt loop**,
not a lock wait. A halted CPU holds no live `waiters` increment, so zero
everywhere is exactly right. **This is the NEGATIVE reading DDR-1062 §5 named
as the valuable half** — *"a frozen CPU with ZERO waiters anywhere now means the
freeze is NOT a lock wait"* — **observed for the first time**, and it means the
lock table separates the two reds on this binary **independently of the RIP**:

| | `[apfreeze]` RIP says | lock table says |
|---|---|---|
| shard 5 | halt loop in `schedule_locked` | 0 spin waiters — not a lock wait |
| shard 7 | spin loop in `spin_lock_contended` | 1 spin waiter — a lock wait |

DDR-1120 §6 separated them by RIP and disassembly. They are now separated a
second time, by an instrument that knows nothing about RIPs. **DDR-1120 §6's
refusal to pool the two is confirmed from a second direction.**

**(b) `g_mounts+0x1C` carries THREE live yield waiters.** That is `mnt_lock`
(`kernel/fs/vfs/vfs.c:45`/`:52` are the only `lock_wait_begin`/`end` callers),
i.e. the lock DDR-994 and `PRE_LAUNCH_CHECKLIST` §4.11 name as the unbounded
wait on OPEN-1 route 1's path, and the lock DDR-1060 §5 built this half of the
instrument to make visible. **RECORDED, NOT ATTRIBUTED.** Three threads piled on
a mount while a CPU is halted is exactly as consistent with *downstream of the
halt* as with anything else, and DDR-1120's capture has a named proximate event
already. No mechanism is claimed and OPEN-1 does not move.

---

## §5 — ONE NEGATIVE, STATED AT ITS REAL WIDTH

The shard-7 capture carries `[percpu] gs OK (syscall ctx)` and
`[percpu] current OK (syscall ctx)` — DDR-SMP-3a's SWAPGS probe **printing a
positive**, not merely being absent, which is more than DDR-1119 could say of
its capture.

**Its width:** that probe runs once, at the top of `syscall_dispatch`, on a
ring-3 syscall entry, and the freeze is on the block path. It says GS was sound
at that instant on that path. It does **not** establish that `this_cpu()` was
sound on the frozen CPU at NMI time, and therefore does **not** by itself
exclude the reading that an NMI snapshot landed in a neighbour's `percpu` block.
What *does* argue against that reading here is simpler and is in the capture:
the `[apfreeze]` `cpu=` index and the `ticks[]` census agree that index 2 is
frozen. Stated because the flattering over-reading is one sentence away.

---

## §6 — WHAT THE NEXT OCCURRENCE NEEDS, AND WHY IT IS **NOT** DDR-1121'S PROPOSAL

DDR-1121 recorded-and-refused a `key=` echo inside `spin_lock_contended` —
*"a change to the hottest primitive in the kernel on the very path OPEN-2 lives
in, the cost DDR-1047 refused and DDR-1060 respected, designed off ONE
occurrence of this site"*. **That refusal stands and is not revisited.** Note
also that as literally worded it is redundant: `ls_slot_for` keys on the lock
address **verbatim** and the dump already prints that key as `lock=`, so an echo
of the slot's key adds nothing unless you first know *which slot the frozen CPU
claimed* — which is per-CPU state, the thing DDR-1060 §3 refused on its own
grounds. DDR-1121 proposed the per-CPU field in one sentence and dismissed it in
another.

**The instrument this capture actually asks for is a different one, and the cost
argument does not apply to it at all: `[apfreeze]` should carry the same
per-CPU tick census `[vblk]` already prints.**

- **It is not on any hot path.** `ap_freeze_probe`'s print arm runs *only after
  a CPU has already frozen*, beside ~20 fields it already emits. Four
  `kputdec`s. DDR-1047's objection is about always-on cost on `spin_lock`; there
  is none here.
- **It is not designed off one occurrence of a rare site.** It closes a
  **structural** limitation — `s_victim` reports one CPU *by construction* — that
  has silently shaped the reading of every OPEN-2 capture ever taken.
- **It changes no behaviour**: no clause, no verdict, no new sentinel; the set
  of conditions that fire is identical.
- **`s_victim` is KEPT.** The single-victim sampling is what makes a pinned RIP
  across four shots mean *spinning*; spreading shots across CPUs would destroy
  that. The census is additive — it says *who else is frozen*, while the shots
  keep saying *what the one we picked is doing*.

**NOT BUILT HERE.** It is a kernel change and it belongs in its own DDR with its
own forced mutant and its own regression run — DDR-1121's discipline, applied to
a proposal that happens to survive the costing rather than fail it. Recorded so
the next session does not re-derive it, and so that `key=` is not built first.

---

## §7 — NOT CLAIMED

- **NO FIX. NO MECHANISM NAMED. OPEN-2 DOES NOT CLOSE** and no open issue moves
  (OPEN-1 / OPEN-2 / OPEN-12 / OPEN-13 untouched). §NON-NEGOTIABLE 3 holds.
- **NO RATE.** One capture with two frozen CPUs; the sibling capture on the same
  binary has one. Nothing is inferred about how often either shape occurs, and
  no campaign was run to manufacture a second.
- **NO CODE CHANGE, NO GATE RUN.** `kernel.bin` is NOT rebuilt, so the
  size/headroom pair and `ci-docstate-check` are unaffected; `GLOBAL_FORBIDDEN`
  **77**; **179** gates; 79 probe ELFs; no new sentinel and no new clause, so the
  set of frames that fire is unchanged.
- **NO DEFECT ALLEGED IN `idt.c`.** `s_victim` is correct for what it was built
  to do and its stated reason is a good one; what is corrected is the **reading**
  that one `[apfreeze]` line means one frozen CPU.
- **NO DEFECT ALLEGED IN `lock_stat.c` or `virtio_blk.c`.** DDR-1121 read both
  and found them correct; this adds nothing against either, and §4(a) is that
  instrument working exactly as designed.
- **DDR-1121 IS NOT WITHDRAWN.** Its four propositions are each measured and each
  stands; its instruction-exact disassembly of both witnesses stands; its finding
  that the dump was already in the capture stands. **One step — the elimination
  in its §4 — is corrected in place**, and the residual contradiction it named is
  narrowed rather than dissolved.
- **DDR-1120 IS NOT WITHDRAWN and is STRENGTHENED**: §4(a) confirms its refusal
  to pool the two reds, from an instrument that knows nothing about RIPs.
- **DDR-1115 / DDR-1119 ARE NOT CORRECTED.** Their captures' censuses do show a
  single frozen CPU, so their inferences hold; §2 records only that the fact was
  read off a latch rather than measured, and that the measurement was available
  in the same lines.
- **§3.1 IS NOT A MECHANISM** and is labelled as a shape, not an attribution.
- **§4(b) IS NOT ATTRIBUTED** and OPEN-1 does not move.
- **`5f3ac9e` IS NEITHER ATTRIBUTED NOR EXONERATED** (DDR-1042).
- **NO INSTRUMENT IS BUILT**, and DDR-1121's `key=` proposal is **not** built and
  **not** endorsed.
