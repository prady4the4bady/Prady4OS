# DDR-1119 — THE FIRST `[schedcheck]` FIRE WAS THE SAME CONSUMED FRAME, AND DDR-1115's OWN MEASUREMENT IS THE PROOF

**Assessment + correction of a reading. Docs-only: no code change, no gate, no new
sentinel, `kernel.bin` NOT rebuilt in the working tree.
NO FIX, NO MECHANISM NAMED, OPEN-2 DOES NOT CLOSE (§NON-NEGOTIABLE 3).**

---

## §0 — PROVENANCE, AND BOTH BINARIES RESOLVED AGAINST THEMSELVES (§INV.18)

| | fire 1 | fire 2 |
|---|---|---|
| CI run / shard / gate | 34742104493 · shard 6 · `smoke-swapgs` (pull_request suite) | 34754906764 · shard 3 · `smoke-nethammer` (push suite) |
| tip | `a5d876e` (docs-only) | `be0b2ad` (docs-only) |
| build job digest | `95493b96c7d13f30`, 1,319,306 B | `f8574d7d6f0ba30e`, 1,319,306 B |
| recorded by | DDR-1115 | DDR-1118 |

**Fire 1's binary was REBUILT BIT-FOR-BIT for this DDR** — a detached worktree at
`a5d876e`, `make image` rc=0, `sha256sum` → **`95493b96c7d13f30`**, the exact digest
that run's build job published. So §2's addresses are measured **in the binary that
produced fire 1's capture**, not carried from fire 2's. The build was deliberately
done **outside the working tree** so `build/kernel.bin` stayed pinned at
`6af029b001e6e6db` — DDR-1060 §9's void-campaign hazard, avoided rather than
rediscovered; re-verified identical afterwards.

Note the two digests differ while the **sizes are identical**. Only the hash
discriminates — DDR-1097's finding arriving for the fifth time in this lineage.

---

## §1 — THE FINDING: DDR-1115 MEASURED THE RIGHT NUMBER AND READ IT BACKWARDS

DDR-1115 §3, on fire 1, computed the offset itself:

> `0x07CB7A70 − 0x07CB7A40 = 0x30 = 48`

and read it as corruption — *"a pointer into the very frame the check was about to
consume, landing on the rbx slot of `context.asm`'s 8-quadword layout."*

DDR-1118 then established, instruction-exactly on fire 2, that **`S+0x30` in slot
`S+0x00` is precisely what CORRECT execution writes there**: `this_cpu`'s `push rbp`,
with `rbp = S+0x30` set by `finish_task_switch`'s own prologue.

**So DDR-1115's own number is the proof of the opposite reading.** The value is not
a stray pointer that happened to land in the RFLAGS slot; it is the exact word the
resume chain deposits at that exact offset. Fire 1 is **the same case as fire 2: a
correct saved `rsp` whose frame has already been consumed.**

This does not make DDR-1115 wrong about anything it *did*. Its two architectural
clauses are correct, they are what caught fire 1, and its enumeration of the stack's
8-aligned addresses is what makes §3's denominators statable. What is corrected is
one sentence of **interpretation** — a reading, not a fault.

---

## §2 — MEASURED IN FIRE 1'S OWN BINARY (`95493b96c7d13f30`)

```
ffffffff80016120 <finish_task_switch>:
ffffffff80016120: 55                pushq  %rbp
ffffffff80016121: 48 89 e5          movq   %rsp, %rbp
ffffffff80016124: 48 83 ec 20       subq   $0x20, %rsp
ffffffff80016128: e8 a3 36 03 00    callq  <this_cpu>        <- at +0x8, so +0xd

ffffffff800497d0 <this_cpu>:
ffffffff800497d0: 55                pushq  %rbp

call context_switch      : EXACTLY 1   @ ffffffff8001669e  -> returns to ffffffff800166a3
call finish_task_switch  : 2           -> OUT OF LINE in this binary
thread_trampoline        : ffffffff800160d0
```

`call finish_task_switch` count is the one that mattered and the reason the rebuild
was worth doing: `finish_task_switch` is `static inline` **in the same translation
unit as `schedule_locked`**, and `a5d876e`→`be0b2ad` changed that TU. Had the
compiler inlined it in fire 1's build there would be no `call` at `S+0x38`, `rbp`
would not land at `S+0x30`, and the prediction would not hold. It is out of line,
with two call sites (`schedule_locked` and `thread_trampoline`) — **measured, not
assumed.**

With `S` the value `context_switch` stored, resuming from `S` therefore writes, in
this binary:

```
[S+0x00] = S+0x30                    (this_cpu's push rbp)
[S+0x08] = 0xffffffff8001612d        (finish_task_switch+0xd)
```

**Fire 1 printed `[S+0x00]` and it read `rsp+0x30` EXACTLY.** It could not print
`[S+0x08]`: `r15=` did not exist until DDR-1116.

### §2.1 — THE NUMERALS MOVE, THE OFFSET DOES NOT

| address | fire 1 binary | fire 2 binary | Δ |
|---|---|---|---|
| after the only `call context_switch` | `0xffffffff800166a3` | `0xffffffff8001673d` | **154** |
| `finish_task_switch+0xd` | `0xffffffff8001612d` | `0xffffffff8001612d` | **0** |

Two binaries whose only source difference is inside `schedule_locked`: one address
moves 154 bytes, the other does not move at all. §INV.18 vindicated in both
directions on one pair of builds — and it is exactly the trap DDR-1116 fell into
(carrying a post-call numeral across a change that moved it by 78) and DDR-1118 §1
corrected. **Re-measure per capture. Never carry the numeral.**

The **offset** `S+0x30`, by contrast, is fixed by the two prologues, and
`git diff a5d876e be0b2ad -- '*.c' '*.h' '*.asm' '*.S' '*.ld' Makefile` touches one
file with its only `finish_task_switch`/`this_cpu`/`context_switch` hit **inside a
comment** — so the chain's source is identical across both fires. That is why the
offset is the thing that carries and the addresses are not.

---

## §3 — STRENGTH, STATED AT ITS REAL SIZE

**The two fires are NOT equal evidence and must not be reported as such.**

* **Fire 2 has TWO independent witnesses** — `[S+0x00] = S+0x30` and
  `[S+0x08] = finish_task_switch+0xd` — each exact, at its own offset.
* **Fire 1 has ONE.** Its single witness is exact and is 1-in-N against the
  corruption alternative, but it is one.

The denominators, and they differ because the *clauses* differed:

| | clause set in that build | stack addresses that would fire | rsp+0x30 is |
|---|---|---|---|
| fire 1 | DDR-1105's mask only | **1920** of 2048 (128 in the low 4 KiB defeat it, DDR-1115 §4) | 1 of 1920 |
| fire 2 | + DDR-1115's architectural clauses | **2048** of 2048 — every 8-aligned address has bit 1 clear, so the reserved-ONE clause catches all of them | 1 of 2048 |

A uniform-corruption model puts the joint coincidence near `2.5e-7`. **That is a
MODEL, not a measurement, and it is not what the claim rests on.** What the claim
rests on is the derivation: the chain has **no free parameter**, it predicts one
specific word at one specific offset, and it has now been measured in **both**
binaries and matched in **both** captures.

---

## §4 — WHAT THE TWO FIRES SHARE, AND WHAT THAT IS AND IS NOT

| | fire 1 | fire 2 |
|---|---|---|
| tid | **22** | **22** (pid 22 — `pid=` did not exist for fire 1) |
| `rsp − base` | `0x3A40` = 14912 | `0x3A10` = 14864 |
| depth above the frame (`ktop − rsp`) | 1472 B | 1520 B |
| RFLAGS slot | `rsp + 0x30` | `rsp + 0x30` |
| heartbeat `ticks[]` / `dest_abs` | `[886,168,863,862]` / 168 | `[865,162,910,783]` / 162 |
| frozen CPU index | **1** | **1** |

The depths differ by **48 bytes** — the same call chain, not the same instruction.

**These are NARROWINGS FOR THE NEXT CAPTURE TO BE READ AGAINST, NOT CONCLUSIONS,
and the reasons are stated rather than left for a reader to supply:**

* **tid 22 twice is weak.** `t->tid = next_tid++` is assigned in boot order, so the
  same probe gets the same tid on every boot of the same sequence. It says *which
  thread* reaches this window, not that the thread is special.
* **CPU 1 twice is weaker still** — 1-in-4 per fire on a `-smp 4` guest.
* **No rate is claimed.** Two fires is two fires.

---

## §5 — NEGATIVE EVIDENCE IN FIRE 2'S CAPTURE, WHICH DDR-1118 DID NOT USE

Read out of the job log rather than inferred. The capture matched **exactly two**
forbidden patterns: `multi-inflight FAIL` and `[schedcheck]`. It carried

* **no `[percpu] gs FAIL`**, **no `[apfreeze]`**, **no `NEXUS KERNEL PANIC`**, **no
  `panic_stage=`**;
* `[hb] t=1000 … rqcpus=3 …` with the guest otherwise healthy — 20 of 21 gates on
  that shard had already passed, and both nethammer probes finished
  `conn_ok=20000 conn_err=0`;
* `[vblk] compl wait timeout unit=3 dest_cpu=1 … dest_abs=162 … ticks[865,162,910,783]`,
  i.e. **the halted CPU's tick count and the stranded completion's are the same
  number** — DDR-1115's causal chain (the block failure is DOWNSTREAM of the halt)
  reproduced on a second artefact.

**So DDR-1010's SWAPGS producer is NOT implicated IN THIS CAPTURE.** Stated at that
width deliberately: that probe runs at the top of `syscall_dispatch`, and this fire
is on the timer/schedule path, so its silence is evidence about syscall entry and
not a general exoneration.

---

## §6 — FOUR CANDIDATES CHECKED AND REMOVED (so they are not re-derived)

The double resume needs a thread to be switched **in** twice with no save between.
Reading the source for how that could happen removed four routes:

1. **`sched_unblock` cannot enqueue a running thread.** The `rq_push` sits inside a
   successful `__atomic_compare_exchange_n(&t->state, THREAD_BLOCKED, THREAD_READY)`;
   a READY or RUNNING thread fails the CAS and `g_ub_cas_fail` counts it.
2. **Cross-CPU dequeue exclusion holds.** `steal_pass` test-and-sets the *victim*
   queue's own `q->lock` before calling `rq_take`, and both `rq_take` and
   `rq_unlink` clear `rq_on` with a RELEASE store under that lock.
3. **`current_thread` is per-CPU**, `#define current_thread (this_cpu()->current)`
   (`sched.h:310`) — not a global two CPUs could share.
4. **`pc->prev` cannot go stale.** It is assigned at `sched.c:1604`, *after* every
   early return in `schedule_locked` (the keep-running return at `:1531` and the
   already-on-idle return at `:1559`), so no non-switching path can leave a prev for
   a later `finish_task_switch` to release prematurely. And the binary contains
   **exactly one** `call context_switch`, so there is no second, unguarded resume
   site.

**NONE OF THESE IS A MECHANISM AND NONE IS CLAIMED AS ONE.** They remove four
candidates; the mechanism is still unnamed and §NON-NEGOTIABLE 3 still forbids a fix.

What is on record and still open: `sched.c:202-207` states the rq-2 exclusion in its
own words — *"Exclusion is the DEQUEUE (a thread sits in exactly one queue, so
exactly one CPU pops it)"* — beside the design note that *"a READY-but-still-on-CPU
thread … is now legitimately takeable"*, guarded by `switch_wait_offcpu_sched`'s
`on_cpu` handshake. That handshake is where a third fire's `rq_on=`/`disp−saves`
will point, or away from.

---

## §7 — WHAT THE SHIPPED INSTRUMENT WILL SAY ON A THIRD FIRE

DDR-1118's fields ship in `5f3ac9e` and existed for **neither** fire:

* **`disp − saves == 2`** — switched in twice with no save between: the consumed-frame
  double resume **confirmed** on an artefact rather than derived from frame bytes.
* **`disp − saves == 1`** — the healthy identity: the reading is wrong and the next
  capture sends it elsewhere. That is the discrimination, and it is why the pair was
  built rather than a flag.
* **`rq_on=1`** — the thread was enqueued while being resumed, i.e. the precondition
  was present. `rq_on=?` means the field itself is untrustworthy (DDR-1092).
* **`ret=`** — re-measure both legal values **against the binary that produced the
  capture**. For the record, in fire 1's binary they are `0xffffffff800166a3` and
  `0xffffffff800160d0`; in fire 2's, `0xffffffff8001673d` and `0xffffffff800160d0`.
  §2.1 is why those tables are per-binary.

---

## §8 — NOT CLAIMED

* **NO FIX, NO MECHANISM NAMED, OPEN-2 DOES NOT CLOSE.** No open issue moves
  (OPEN-1/2/12/13 untouched).
* **NO RATE.** Two fires, and §4 states why the two shared attributes are weak.
* **NO CODE CHANGE.** `build/kernel.bin` is `6af029b001e6e6db`, 1,319,306 B,
  verified unchanged after the worktree build — so the size/headroom pair and
  `ci-docstate-check` are unaffected. `GLOBAL_FORBIDDEN` 77, 179 gates, 79 probe
  ELFs, no new sentinel, no gate arm.
* **DDR-1115 IS NOT WITHDRAWN OR CRITICISED.** Its clauses are correct, they are what
  caught fire 1, and its own measurement is what proves this. One sentence of
  interpretation is corrected in place.
* **DDR-1118 IS NOT EXTENDED.** It analysed fire 2 and deliberately did not revisit
  fire 1; this asks that question and answers it separately, with fire 1's binary
  rebuilt for the purpose.
* **THE WORKTREE BUILD IS A MEASUREMENT AID, NOT A RELEASE ARTEFACT.** It was built
  outside the working tree precisely so the pinned binary stayed pinned, and it is
  removed afterwards.
* **NO GATE WAS RUN for this DDR.** What was measured: the two captures' raw fields
  re-computed from the numerals, the `a5d876e` rebuild and its digest, that binary's
  disassembly of `finish_task_switch` / `this_cpu` / the `call context_switch` site /
  `thread_trampoline`, the `call finish_task_switch` count, the `a5d876e..be0b2ad`
  build-input diff, the fire-2 job log read in full, and the four source facts in §6.
