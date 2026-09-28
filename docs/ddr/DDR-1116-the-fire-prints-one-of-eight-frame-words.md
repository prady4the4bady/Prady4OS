# DDR-1116 — THE FIRE PRINTS ONE WORD OF AN EIGHT-WORD FRAME, AND THE SHARPEST WITNESS IS THE ONE IT OMITS

**Status:** instrument change only. **NO FIX. NO CAUSE NAMED. OPEN-2 DOES NOT CLOSE**
(§NON-NEGOTIABLE 3). No new gate, no new sentinel, no new clause.

---

## §0 — Provenance

DDR-1105 built the `next->rsp` validity check; DDR-1106 extended it to idle threads;
DDR-1115 recorded its **first fire ever** and added two architectural RFLAGS clauses.
This is the next step in that lineage and it is about **what the fire can SAY**, not
about what it can CATCH.

DDR-1115 §1 took its artefact as far as one sentence — *"clauses 2 and 3 PASSED …
only the frame's CONTENT is wrong"* — and **could go no further**, because the halt
line prints exactly **one** word of the frame.

---

## §1 — THE FINDING: the instrument delivers strictly LESS than the panic dump it replaced

`context_switch` saves **eight** quadwords (`arch/x86_64/context.asm:53-59`, read from
the source, not from a comment):

| offset | slot |
|---|---|
| `+0x00` | RFLAGS ← **the only word the fire prints** |
| `+0x08` | r15 |
| `+0x10` | r14 |
| `+0x18` | r13 |
| `+0x20` | r12 |
| `+0x28` | rbp |
| `+0x30` | rbx |
| `+0x38` | **return address** |

DDR-1099, working from a **full panic dump**, had **two independent witnesses** —
RFLAGS *and* R15 — and that is precisely what let it write *"two independent slots of
one frame hold values from elsewhere on that stack"*. DDR-1115, working from the
instrument, had **one**, and its §2 had to concede that the reading of which CPU
halted *"is an inference"*.

So the instrument built to replace an undiagnosable failure with a named line is, on
the one axis that matters most, **weaker than the dump**. That is the defect corrected
here, and it is the DDR-1046/1060/1074/1080/1092 class arriving in this lineage for
the second time: an instrument that cannot discriminate the case it exists for.

---

## §2 — WHY THE RETURN SLOT, AND WHY IT IS THE SHARPEST AVAILABLE

Because its set of legal values has **exactly two members**, and both are **measured
in the current binary** rather than carried from a previous one (§INV.18: *an address
does not identify a binary*). `build/kernel.bin` = `4bcbdf1bd1cea57f`:

1. **`0xffffffff800166ef`** — there is **exactly one** `call context_switch` in the
   whole kernel (`llvm-objdump -d build/kernel.elf | grep 'call.*<context_switch>'`
   returns **one line**, at `0xffffffff800166ea`, five bytes). Every frame saved by a
   live switch therefore carries that return address and no other.
2. **`0xffffffff800160d0`** = `&thread_trampoline` — the seed at `sched.c:1182`, the
   **only** place in the tree that writes a return address into a frame
   (`grep -rn thread_trampoline` over `kernel/` and `arch/`: one frame-writing site).

**The enumeration was checked for a third writer and there is none.** `fork` does not
construct a frame — it goes through `sched_create_user` → `sched_create_state`, i.e.
**the same seed**, and stores its register set in `->fork_regs` for
`signal_sigreturn()` on the `user_launch` path (`sched.c:1234`), which is a *ring-3*
resume and not a switch frame at all.

A two-valued field is the strongest discriminator the frame offers. RFLAGS, by
contrast, is legitimately variable (DDR-1105 §4: the arithmetic flags and IF are
deliberately unmasked), which is exactly why its test needed three clauses and why
DDR-1115 §3 had to measure a 128-of-2048 blind spot in one of them.

---

## §3 — WHAT IT DISCRIMINATES: two live hypotheses that are TODAY THE SAME OBSERVATION

After DDR-1115 the artefact is consistent with **both** of these, and the printed line
cannot separate them:

* **(A) A REAL frame with corrupt content.** `next->rsp` points at the frame
  `context_switch` genuinely saved, and one or more words in it were overwritten.
  → the return slot would read **one of the two legal values**.
* **(B) NOT A FRAME AT ALL.** `next->rsp` points at a location that is inside the
  thread's own stack window (so clauses 2 and 3 pass, exactly as observed) but was
  never the base of a saved frame — a stale pointer, a frame that has since been
  overwritten wholesale, or a stack whose memory was freed and reissued.
  → the return slot would read **neither**.

That distinction is the difference between *"the scheduler's own frame was damaged"*
and *"the pointer is stale"*, which are different defects with different fixes. **One
load answers it.** Under (A) the next question is *what wrote into a live frame*;
under (B) it is *what made the pointer stale* — and DDR-1096 §3's unlocked ring walk
and DDR-996's freed-while-queued family live under (B) only.

`r15` (`+0x08`) is printed **beside** it for the same reason DDR-1099 used it: a
second, independent witness. Under (A) it holds a callee-saved value; under the seed
it is **0**, because the seed writes zero to all six GP slots (`sched.c:1183-1188`).

---

## §4 — SAFETY: the loads are already proved safe by the clauses that ran first

This is **not** a new argument; it is DDR-1105's own, applied unchanged. Clause 3
has already established

```
nrsp + 64u <= nbase + STACK_SIZE      and      (nrsp & 7u) == 0
```

*before* any dereference — that bound exists precisely because the frame is 64 bytes,
so `[nrsp+0x08]` and `[nrsp+0x38]` are **inside the same window** clause 3 proved for
`[nrsp+0x00]`. Bounds and alignment are what make the load safe, which is why the
clause order is load-bearing and is not disturbed: reversing it would turn a detector
into a second fault source, the defect DDR-1079 fixed in the panic backtrace walker.

The reads are `volatile` and the values are **printed, never dereferenced** — the
DDR-1079 rule that a distrusted pointer may be reported but not followed.

---

## §5 — COST: the hot path pays ZERO

Both loads and both `kline_x` calls sit **inside `if (bad)`**, a branch never taken on
a healthy boot. This is not the shape DDR-1047 refused (an `rdtsc` **pair** on every
one of ~1.9M `spin_lock` acquisitions); it is strictly less than DDR-1106, which added
nothing to the hot path either. **The check itself is untouched** — same clauses, same
order, same constants.

---

## §6 — NO NEW CLAUSE. THIS IS THE LOAD-BEARING REFUSAL

It is tempting to make the return slot a **fourth clause** — "must be one of two
values" — and it is **refused**.

DDR-1115 §4 admitted its two new clauses on one specific ground: they hold **by the
ISA** (bit 1 is reserved-one; bits 22..63 are reserved-zero), so they *cannot*
false-positive on any legitimate `pushfq` on any x86_64 machine. A return-address test
has **no such standing**. It would hold only by an enumeration of *today's tree* — a
**POLICY test about this kernel's habits**, which is exactly what DDR-1115 §3
criticised in the `TF|DF|IOPL` mask. A future signal, exec or fork path that seeds a
frame differently would falsify it **silently**, and the cost of being wrong here is
**a halted CPU on the hottest path in the kernel**.

So: **PRINT, DO NOT JUDGE.** The set of frames this fires on is **unchanged**; only
what the line says about them changes. A value outside the two-member set is
informative to a *reader* and must not be fatal to a *machine*.

---

## §7 — LINE BUDGET: measured, not argued

`KLINE_MAX` is **256** (`kernel/console.h:23`), and an overflow emits `[kline] TRUNC`,
which is in `GLOBAL_FORBIDDEN` — so a careless addition would **redden the run and
destroy the artefact it was added to capture**. Worst case, with `kline_x` at 18 chars
(`"0x"` + 16 digits) and `tid`/`pid` both `uint32_t` (≤ 10 digits each,
`sched.h:51/:66`):

```
35 (prefix) + 10 (tid) + 5+10 (pid) + 5+18 (rsp) + 6+18 (base)
   + 8+18 (rflags) + 5+18 (r15) + 5+18 (ret) + 11 (" halting.\r\n")  =  190
```

**190 of 256, 66 to spare, at a worst case the real fields cannot reach.** The mutant
run measures the actual emitted length rather than trusting this arithmetic.

---

## §8 — PROOF, AND THE VACUITY CHECK DONE FIRST

**The obvious arm is vacuous** (nineteenth time caught in design text): *"force a fire
and check that `ret=` appears"* proves only **wiring**. What must be shown is that the
field **DISCRIMINATES**, i.e. that it takes **different values under the two
hypotheses of §3**. Two mutants, each changing **one constant** (DDR-1042), on
recorded hashes:

* **M1 — hypothesis (A), a real frame with one corrupt word.** `sched_create`'s seed
  `0x202` → `0x302` (TF set), DDR-1105's own M1. The frame is otherwise genuine and
  freshly seeded, so the prediction is **`ret=0xffffffff800160d0`** (`thread_trampoline`,
  legal value #2) and **`r15=0`** (the seed's zero).
* **M2 — hypothesis (B), the pointer does not address a frame.** `t->rsp = sp - 0x40`,
  which is 8-aligned and in-bounds so clauses 2 and 3 still pass, but places the
  window **below** the seeded frame: `[rsp+0x38]` reads `[sp-8]`, heap memory the seed
  never wrote (and `kmalloc` does not zero — §NON-NEGOTIABLE 10). The prediction is
  **`ret=` neither legal value**.

**The claim is exactly that M1 and M2 print different CLASSES of `ret=`.** If they
printed the same thing the field would be decoration, and this DDR would be wrong.
Both controls (pre-change builds) must fire identically **without** the new fields, so
the delta is the fields and nothing else.

---

## §9 — AN OBSERVATION ABOUT THE CURRENT BINARY, EXPLICITLY **NOT** A RE-ATTRIBUTION

`thread_trampoline` opens `pushq %rbp; movq %rsp, %rbp; subq $0x10, %rsp` — **it
establishes a frame pointer** — and `thread_trampoline+0xd` = `0xffffffff800160dd` is
the return address of its `call finish_task_switch`.

DDR-1099's artefact read `R15 = 0xFFFFFFFF800160DD` and resolved it, **against its own
binary** `0693e5b04685ad60`, as `finish_task_switch+0xd`. In the **current** binary
`finish_task_switch` sits at `0xffffffff80016120`, so `+0xd` is `0xffffffff8001612d`
and the same numeral resolves elsewhere. **THAT IS NOT A CORRECTION TO DDR-1099 AND IS
NOT OFFERED AS ONE** — re-resolving another binary's address against this symbol table
is precisely the error §INV.18 exists to prevent, and DDR-1099 followed the correct
discipline. It is recorded for one narrow reason: **both readings name the same
structural thing**, a return address left by a *previous* activation on that stack,
which is what DDR-1099 itself concluded. And the trampoline **pushing rbp** is the
shape of the value DDR-1115 observed — a frame-pointer-like word in the RFLAGS slot.
**A matching shape is not a mechanism** (DDR-1056), and none is named.

---

## §10 — NOT CLAIMED

* **NO FIX**, **NO mechanism named**, **OPEN-2 does not close**, **no rate claimed** —
  one fire, ever.
* **NO new clause**, so **the set of frames that fire is UNCHANGED**; this changes only
  what the line reports. A wrong `ret=` will **not** halt anything.
* **NO defect found in the scheduler and none alleged** — `context_switch`, the frame
  layout, the seed and the kick path are all correct.
* **NOT exonerated in advance** (DDR-1042): this edits the same function as DDR-1115,
  so if the OPEN-2 signature moves, this commit is a candidate and *"the diff is
  elsewhere"* is not an argument.
* **DDR-1105 and DDR-1115 are NOT criticised.** Their clauses are correct and are kept
  verbatim; §1 records that the *line*, not the *check*, was the weaker half — a
  measurement, not a fault.
* **DDR-1099 is NOT corrected** (§9).
* **No new gate** (179 unchanged) and **no gate arm** — DDR-1105 §8's reason is
  unchanged: the triggering condition cannot be manufactured in product, and asserting
  the ABSENCE of a rare intermittent is unfalsifiable at any affordable N (the shape
  DDR-1082 costed and refused).
* **`GLOBAL_FORBIDDEN` 77 unchanged** — `[schedcheck]` is already entry 75 and the line
  keeps its prefix.
* **No open issue moves** (OPEN-1/2/12/13 untouched); this is not an apfreeze.

---

## §11 — MEASURED RESULTS

Clean build `f8574d7d6f0ba30e`, **1,319,306 B — SIZE UNCHANGED** from DDR-1115's
`4bcbdf1bd1cea57f` (the addition fits inside existing page padding). **A size
comparison cannot tell the two binaries apart at all; only the hash discriminates** —
DDR-1097's finding arriving for the third time in this lineage. The size/headroom pair
and `ci-docstate-check` are therefore unaffected.

### §11.1 — The discrimination, two mutants, two kernels

```
M1  hash=e89ef5d01e8679ec  rc=2  SCHEDCHECK=1     (hypothesis A: real frame, one word corrupt)
    [schedcheck] next->rsp invalid tid=1 pid=0 rsp=0x0000000007EC3FC0
      base=0x0000000007EC0000 rflags=0x0000000000000302
      r15=0x0000000000000000 ret=0xFFFFFFFF800160D0 halting.

M2  hash=8f9a6e8bb7a2ad4d  rc=2  SCHEDCHECK=1     (hypothesis B: not a frame at all)
    [schedcheck] next->rsp invalid tid=1 pid=0 rsp=0x0000000007EC3F80
      base=0x0000000007EC0000 rflags=0x0000000000000000
      r15=0x0000000000000000 ret=0x0000000000000000 halting.
```

**`ret=` TOOK DIFFERENT CLASSES, WHICH IS THE ENTIRE CLAIM.** M1 reads
`0xFFFFFFFF800160D0` — `&thread_trampoline` **exactly**, legal value #2, as predicted
for a genuine freshly-seeded frame, with `r15=0` from the seed and `rsp-base = 0x3FC0
= STACK_SIZE-64` confirming the frame sits where the seed puts it. M2 reads **`0x0`,
which is neither legal value**, from a window placed `0x40` below the seeded frame.
Had both printed the same thing the field would be decoration and this DDR would be
wrong.

**A weakness in M2 stated rather than glossed:** its `ret` reads **zero** because that
heap region happened to be zero, not because anything guarantees it — `kmalloc` does
not zero (§NON-NEGOTIABLE 10), so the value there is whatever a previous owner left.
Zero is still *not one of the two legal return addresses*, so the discrimination holds;
but M2 demonstrates "neither legal value" with a **tidy** witness rather than a
garbage one, and a reader should not infer that the out-of-frame case reads zero in
general.

**`LINELEN=170`** measured on both, against `KLINE_MAX` 256 — 86 to spare, and §7's
worst-case estimate of 190 was conservative in the right direction. No `[kline] TRUNC`.

### §11.2 — READ THESE TWO ADDRESSES OUT OF THE BINARY THAT PRODUCED THE CAPTURE

For `f8574d7d6f0ba30e` the legal return-slot values are

```
0xffffffff800166ef   after the unique `call context_switch`
0xffffffff800160d0   &thread_trampoline (the seed)
```

**Both are binary-specific and MUST be re-measured per capture (§INV.18: an address
does not identify a binary).** The commands are
`llvm-objdump -d build/kernel.elf | grep 'call.*<context_switch>'` and
`nm -n build/kernel.elf | grep thread_trampoline`. A future reader who compares a
capture's `ret=` against the numerals above rather than against its own binary will
reach a confident wrong answer, which is the exact failure §INV.18 names.

### §11.3 — CI on the DDR-1115 binary, recorded because it is OPEN-2 evidence

`33fa80a` took **both** suites green — push `34752528593` and pull_request
`34752530784`, **20 shard jobs, all SUCCESS**, on binary `4bcbdf1bd1cea57f`. So
DDR-1115's two new architectural clauses ran across every shard on real CI timing with
**ZERO false positives**, which was that DDR's load-bearing safety claim. **And no
second `[schedcheck]` fire**: still ONE occurrence in the project's history, **no rate
claimed**, and no campaign run to manufacture one.

### §11.4 — A THIRD MEASUREMENT DEFECT OF MY OWN, recorded not quietly fixed

The first `make image` after the edit reported the hash **UNCHANGED**, exactly as in
DDR-1115 §7.1(a) — but the cause was different and worse, because `build/sched.o` had
already been removed. The build command was piped as
`make image 2>&1 | grep -iE 'error|warning' | head -20`, and **every compile line
contains `-Werror`**, so the grep matched ~300 lines, `head -20` closed the pipe, and
**SIGPIPE killed `make` mid-build**: the link never ran, and the stale `kernel.bin`
read back with its old hash. That is DDR-1048's pipeline lesson arriving in my own
tooling for the second time in one session.

Two rules carried forward: **never put `head` in a pipeline whose head is a build**,
and **match diagnostics on the word boundary `\b(error|warning):`, never on the
substring**, or the compiler's own `-Werror` flag makes every clean line look like a
failure. Corrected to `make image > LOG 2>&1` with the exit code and
`grep -c 'kernel/proc/sched\.c'` both checked — which is how the second attempt proved
`sched.c` genuinely recompiled rather than assuming it.

**Caught only by hashing.** The design's own instruction — never trust `make`, verify
by hash — is the only reason this was not "verified" against a binary that did not
contain the change.
