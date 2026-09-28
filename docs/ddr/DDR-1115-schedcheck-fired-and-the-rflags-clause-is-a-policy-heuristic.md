# DDR-1115 — THE `[schedcheck]` INSTRUMENT FIRED, AND ITS RFLAGS CLAUSE IS A POLICY HEURISTIC WHERE TWO ARCHITECTURAL INVARIANTS WERE AVAILABLE

**Status:** artefact captured + detector strengthened.
**NO FIX. NO CAUSE NAMED. OPEN-2 DOES NOT CLOSE.** (§NON-NEGOTIABLE 3.)

---

## §0 — PROVENANCE, READ NOT INFERRED

CI **34742104493** (the `pull_request` suite), job **103683640809**,
`build-and-boot (shard 6)`, head `a5d876e`. The shard died at
`smoke-swapgs` after 4 of 17 gates and its post-gate step still printed
`kernel.bin: OK`.

**The binary is established, not assumed.** `a5d876e`'s own build job
(**103683430175**) published verbatim:

```
95493b96c7d13f30d83bb116d6d9157922081db827ee30d61ca971f7d6674d94  kernel.bin
kernel: build/kernel.bin (1319306 bytes)
probe-rodata-check: OK — 77 ELFs, none carry a writable allocated section
```

`sha256sum build/kernel.bin` on this host reads the **same digest at the same
size**, so the artefact is about the exact binary sitting on disk here. That
digest is also `83ac6e5`'s and `ff80e56`'s: **six consecutive docs-only commits
have not moved the kernel.**

**AND THE SAME BINARY WENT GREEN IN THE OTHER SUITE.** Run **34742101390**
(`push`, same commit) has all ten shards `success`, **shard 6 included**
(job 103683603193). One binary, two outcomes, one run apart — the DDR-1009
class, and the reason no build regression is alleged here.

---

## §1 — THE ARTEFACT

```
[schedcheck] next->rsp invalid tid=22 rsp=0x0000000007CB7A40
             base=0x0000000007CB4000 rflags=0x0000000007CB7A70 halting.
```

**This is the first time the DDR-1105/1106 instrument has ever fired.** It was
built as DDR-1099 §7's named next step, shipped with forced mutants because its
triggering condition cannot be manufactured in product, and had until now
produced nothing but silence across every suite.

### §1.1 — Which clause tripped, and which did not

Measured, not read off the line:

| clause | value | verdict |
|---|---|---|
| 1. `kstack_base != 0` | `0x07CB4000` | covered (DDR-1106) |
| 2. 8-aligned | `rsp & 7 == 0` | **PASSED** |
| 3. in `[base, base+STACK_SIZE)` with room for 64 B | `rsp-base = 0x3A40 = 14912`, `+64 ≤ 16384` | **PASSED** |
| 4. RFLAGS slot `& (TF\|DF\|IOPL)` | `0x07CB7A70 & 0x3500 = 0x3000` → **IOPL = 3** | **TRIPPED** |

So the pointer is **not garbage in the wild sense**. It is 8-aligned, it lies
inside tid 22's own kernel stack, it leaves room for the whole frame, and the
stack base is 16 KiB-aligned exactly as `pmm_alloc_pages(2)` hands them out.
`rsp` sits 1,472 bytes below the top of the stack, i.e. this thread had run and
descended a few frames — it is not a freshly seeded thread (which would be at
`base + STACK_SIZE - 64`).

**Only the CONTENT of the frame is wrong**, which is narrower than DDR-1099's
artefact and is the shape that DDR named: *"next->rsp did not point at the frame
`context_switch` saved."*

### §1.2 — The RFLAGS slot holds a stack address, and it is 48 bytes above `rsp`

```
0x07CB7A70 − 0x07CB7A40 = 0x30 = 48
```

The word at `[rsp + 0]` — the slot `pushfq` fills, because `pushfq` is the last
push (which is precisely why DDR-1099's faulting `popf` was the first restore
instruction) — **holds an address inside this very stack, pointing 48 bytes
further up the same frame.** Against `context.asm`'s 8-quadword layout
(`RFLAGS, r15, r14, r13, r12, rbp, rbx, return address` at `+0x00 … +0x38`),
`+0x30` is the `rbx` slot — **verified by reading `arch/x86_64/context.asm` itself** (`push rbx, rbp, r12, r13, r14, r15, pushfq`), not taken from the comment beside the check, per DDR-1105's own "read off context.asm's push/pop sequence, not assumed".

**TWO INDEPENDENT ARCHITECTURAL FACTS SAY THIS WORD NEVER CAME FROM `pushfq`,
and neither of them is the clause that caught it:**

* **RFLAGS bit 1 reads as 1 always** on x86_64. `0x07CB7A70` has bit 1 **clear**.
* **RFLAGS bits 22–63 are reserved and read as zero.** `0x07CB7A70 >> 22 = 0x1F`
  — **set**.

`sched_create` seeds the constant `0x202`, which satisfies both (bit 1 set,
upper bits zero), and a genuine `pushfq` satisfies both by construction. So the
observed word is disqualified twice over by guarantees the check does not test.

**Naming which corruption produced it is NOT attempted.** One line does not
carry a RIP, and §NON-NEGOTIABLE 3 forbids a fix on a mechanism that has not
been named. What is established is the shape: the frame boundary is misplaced
or the frame is leftover, and the word at the RFLAGS slot is stack data.

---

## §2 — THE CAUSAL CHAIN IS VISIBLE IN ONE CAPTURE FOR THE FIRST TIME

The same capture matched a second forbidden pattern:

```
[blk] multi-inflight FAIL done=0x0000000000000000 spawned=2/2
[vblk] compl wait timeout unit=0 dest_cpu=1 dest_dticks=0 dest_abs=168
       bsp_abs=886 dest_present=1 ticks[886,168,863,862] on_cpu=0 lba=1
```

**`dest_abs=168` and `ticks[886,168,863,862]` are the same number.** CPU 1's
tick count is frozen at **168** while CPUs 0/2/3 reach 886/863/862, and
`dest_dticks=0` says it did not advance while the submitter waited. The
`[schedcheck]` line's trailing context in the capture is `[boot-load]
COMPOSIT.ELF t=189`, so it was emitted at roughly `t ≤ 189`.

So: **a CPU halted itself with a named line at ~t=168; the virtio-blk
completion routed to that CPU then stranded; `multi-inflight FAIL` followed.**
Every capture of this family before today ended at the *downstream* symptom —
DDR-1079 recorded that captures ending in `blk integrity FAIL` / `compl wait
timeout` had been read as the primary event. This one names the primary event
and the symptom in the same file.

**STATED AS A NARROWING, NOT A PROOF.** `[schedcheck]` does **not print its own
CPU**, so "the halted CPU is CPU 1" is an inference from three facts (exactly
one CPU is frozen; its tick count matches `dest_abs`; the halt appears at a
consistent point in the capture). It is the only reading consistent with the
capture, and it is still an inference. §6.2 records why the CPU is not printed.

---

## §3 — THE FINDING: THE MASK IS A POLICY ARGUMENT WHERE A GUARANTEE WAS AVAILABLE

DDR-1105 chose `MASK = TF|DF|IOPL (0x3500)` and justified each bit **separately
and correctly**, as a statement about *this kernel*: TF is set only by a
debugger and there is none; `std` appears nowhere in the tree so DF is never
set; kernel threads run at IOPL 0 and nothing writes `RFLAGS.IOPL`. The
arithmetic flags and IF were deliberately left out because `pushfq` captures
whatever the outgoing thread left.

That reasoning is sound and is **not** withdrawn. What it is, is a **policy
heuristic** — it asks *"could this kernel have produced these bits?"* Two
**architectural invariants** were available beside it, asking the strictly
stronger question *"could any `pushfq` on this ISA have produced this word?"*,
and they were not used.

### §3.1 — The gap is measured on the observed word class, not modelled

A corrupt RFLAGS slot is not a uniformly random word — this one is a **stack
address**, which is highly structured. So the right measurement is over that
class. Enumerating every 8-aligned address inside tid 22's own 16 KiB stack:

| clause | same-stack addresses that **defeat** it |
|---|---|
| `& 0x3500` (current) | **128 of 2048 — 6.2%** |
| bit 1 must be set | **0 of 2048** |
| bits 22–63 must be zero | **0 of 2048** |

The 128 that defeat the mask are exactly `[0x07CB4000, 0x07CB4AF8]` — **the low
4 KiB of the stack**, where bits 12/13 (IOPL) are clear. Bits 8 and 10 do the
rest.

**So this is not a theoretical hole.** Had the corrupt pointer landed in the
lower quarter of its own stack, clause 4 would have passed it, `schedule_locked`
would have called `context_switch`, and the machine would have taken the corrupt
frame — DDR-1099 §6's *"undiagnosable jump into nowhere"*, which is the exact
outcome this instrument exists to prevent. **The check caught this occurrence
partly by where the pointer happened to land.**

By contrast, **every** address in low kernel memory has bits 22–63 non-zero
(this stack is at `0x07CB_xxxx`, so `>> 22` is `0x1F`), so the reserved-bits
clause is decisive for the whole class rather than probabilistically good.

### §3.2 — The two clauses cannot produce a false positive

This is the load-bearing safety claim, because a false positive here **halts a
CPU on the hottest path in the kernel**:

* **bit 1 set** — x86_64 defines RFLAGS bit 1 as reserved, reading 1. `pushfq`
  therefore always stores it set. `sched_create`'s seed `0x202` has it set.
* **bits 22–63 zero** — architecturally reserved, read as zero; `pushfq` stores
  a 64-bit value whose top 42 bits are zero. `0x202` satisfies it.

Both hold for **every** legitimate value that can occupy that slot, by the ISA
rather than by an argument about this kernel's habits. They are therefore free
in the sense that matters: they can only fire on a slot that is already not a
`pushfq` result.

---

## §4 — WHAT SHIPS

Two changes, both small, both in `schedule_locked`.

1. **Clause 4 gains the two architectural tests.** The heuristic mask is
   **kept**, not replaced: it is the clause that caught this occurrence, its
   per-bit justification still stands, and dropping it would narrow coverage on
   a word that is flags-shaped but has a bit this kernel never sets. The three
   are OR-ed into one condition; **the printed `rflags=` value tells a reader
   which of them tripped**, the same way DDR-1105/1106's two mutants were told
   apart by that field rather than by adding a second field.

2. **`pid=` joins the halt line.** `tid=22` is currently **unresolvable** —
   nothing else in any capture prints a tid, so the artefact above cannot say
   which thread it was. `pid` is a plain `uint32_t` in the same tcb the check
   already reads three fields from, and pids appear throughout every boot log
   (`[user] sys_exit(0) pid=46`, `[svc] start exectest pid=50`), so it makes the
   halted thread correlatable. `pid == 0` is itself informative: kernel threads
   keep pid 0 (`sched.c`), so zero says "kernel thread" rather than "unknown".

**Hot-path cost:** two additional compares and branches on a path DDR-1105
measured at three loads, an add, five compares and five branches — never taken
on a healthy boot and so perfectly predicted. No new loads: the RFLAGS word is
already in a register. `pid=` is read **inside the `if (bad)` branch**, which
never executes on a healthy boot, so it costs the hot path nothing.

### §4.1 — REFUSED, with reasons

* **Printing the thread NAME.** `struct tcb` carries `const char *name`, a
  **pointer** (`sched.c:1117`), not an inline buffer — `name_buf[16]` is empty
  until `SYS_SETNAME`. Dereferencing a pointer out of a tcb the check has just
  declared untrustworthy, through a `kline_s` that walks to a NUL, is the
  **DDR-1079 defect exactly**: the panic backtrace walker faulted mid-report and
  cost the machine a CPU. A bounded copy does not help, because the risk is the
  *pointer*, not the length. Validating it against the rodata range is
  machinery on the one path that runs on untrusted state. **So `tid=` stays
  unresolvable, and that is recorded as a limitation rather than closed.**
* **Printing the CPU.** `this_cpu()` reads `%gs:0`, and a broken SWAPGS
  discipline is one of OPEN-2's **own** producers (DDR-1010) — a bad GS base
  dereferenced here yields no line at all. `lapic_id()` avoids GS but is invalid
  pre-LAPIC (DDR-1055 refused a per-CPU console guard for that reason), and a
  plausible wrong CPU id is worse than none (DDR-1092: a broken measurement must
  never read as a confident answer). §2 shows the correlation is derivable from
  `dest_abs=` against the heartbeat's `ticks[]` anyway.
* **Printing `prev->tid`.** Speculative — no artefact suggests the outgoing
  thread matters, and DDR-1069's test applies: nothing needs it yet.

---

## §5 — PROOF

The triggering condition cannot be manufactured in product, so the proof is
**forced mutants on recorded hashes**, the DDR-1105/1106 standard. Each mutant
changes **one** constant (DDR-1042: attribution from a mutation that changed two
things is the failure mode).

Both mutants seed `sched_create`'s frame with a word that the **current** mask
**passes**, so each is *silent before and firing after* — which is the whole
claim. Both are safe to boot: `popf` ignores reserved bits, so neither faults.

| mutant | seed | clause it defeats today | expected pre-change | expected post-change |
|---|---|---|---|---|
| **M1** | `0x202` → `0x0000000004000202` | bit 26 set; mask bits clear, bit 1 set | `SCHEDCHECK=0`, silent | fires on **bits 22–63** |
| **M2** | `0x202` → `0x0000000000000200` | bit 1 clear; mask bits clear, upper zero | `SCHEDCHECK=0`, silent | fires on **bit 1** |

They land on **different** new clauses and neither carries the other; the
printed `rflags=` value distinguishes them.

**The negative is the load-bearing half**, because a false positive halts the
machine on the hottest path: on the clean kernel the SMP-heavy gates must be
`rc=0` with **zero** `[schedcheck]`, and the hash must be re-verified unchanged
after the last run (DDR-1060 §9).

Results are recorded in §7 after the runs.

---

## §6 — NOT CLAIMED

* **NO FIX, and OPEN-2 DOES NOT CLOSE.** No mechanism is named for why
  `next->rsp` pointed at a non-frame. What changes is the set of corrupt words
  the *next* occurrence can be caught on.
* **NO RATE.** One fire, ever. The instrument shipped at DDR-1105 and this is
  its first. No campaign was run to manufacture a second.
* **NOT ATTRIBUTED TO `a5d876e`, AND NOT EXONERATED EITHER** (DDR-1042). The
  commit is docs-only and the published digest proves the binary did not move,
  so this is boot-side and not a build regression — but "the diff is elsewhere"
  is not an argument, and the same binary passed shard 6 in the sibling suite.
* **NOT ATTRIBUTED TO DDR-1096 §3** (the unlocked ring walk in the timer ISR).
  A stale `next->rsp` is *consistent* with a reissued TCB, and **a matching
  mechanism is not an attribution** (DDR-1056). The OPEN2_HUNT harness has still
  never fired outside its forced build.
* **NO DEFECT IS FOUND IN THE SCHEDULER AND NONE IS ALLEGED.** `context_switch`,
  the frame layout, `sched_create`'s seed and the kick path are all correct;
  what is added is a stricter test on a value that should never be wrong.
* **DDR-1105 IS NOT CRITICISED.** Its mask is correct, its per-bit reasoning
  still holds, it is the clause that caught this occurrence, and it is **kept**.
  §3 records that a strictly stronger test was available beside it — which is a
  measurement, not a fault.
* **NOT EXONERATED IN ADVANCE** (DDR-1042): this changes the hottest path in the
  kernel, so if the OPEN-2 signature moves, this commit is a candidate.
* **NO NEW GATE** (179 unchanged) and **no gate arm** — DDR-1105 §8's reason is
  unchanged: the triggering condition cannot be manufactured in product, and
  asserting the *absence* of a rare intermittent is unfalsifiable at any N this
  project can afford (the shape DDR-1082 costed and refused).
* **`GLOBAL_FORBIDDEN` 77 UNCHANGED** — `[schedcheck]` is already entry 75, and
  this capture is the proof that works: the line reddened `smoke-swapgs`, a gate
  that has nothing to do with the scheduler, exactly as intended.

---

## §7 — MEASURED RESULTS

Design fixed in §5 **before** the runs. Four distinct kernel hashes.

| case | check | seed | kernel | rc | `[schedcheck]` |
|---|---|---|---|---|---|
| **M1-pre** | old | `0x04000202` | `12f799eb4147c5c7` | **0** | **0 — SILENT** |
| **M1-post** | new | `0x04000202` | `0d14f515bf1effce` | 2 | **1 — FIRES** |
| **M2-pre** | old | `0x200` | `7ee6407bfe4311ca` | **0** | **0 — SILENT** |
| **M2-post** | new | `0x200` | `6dc4d07fb0367123` | 2 | **1 — FIRES** |

```
M1-post: [schedcheck] next->rsp invalid tid=1 pid=0 rsp=0x0000000007EC3FC0
                      base=0x0000000007EC0000 rflags=0x0000000004000202 halting.
M2-post: [schedcheck] next->rsp invalid tid=1 pid=0 rsp=0x0000000007EC3FC0
                      base=0x0000000007EC0000 rflags=0x0000000000000200 halting.
```

**BOTH PRE-CHANGE CONTROLS PASSED THEIR GATE — `rc=0`, ~50 KB captures.** That is
the claim, measured rather than argued: on today's tree a frame whose RFLAGS slot
carries `0x04000202` or `0x200` is **taken**, silently, and the gate goes green.

**The two land on DIFFERENT clauses and neither carries the other** (DDR-1044),
and the printed `rflags=` value is what distinguishes them, exactly as §4
designed: M1's word has `& 0x3500 == 0` and bit 1 **set**, so only the
**reserved-bits** clause can have tripped; M2's has `& 0x3500 == 0` and
`>> 22 == 0`, so only the **bit-1** clause can have.

**`pid=` works and reads `0`** — tid 1 is a kernel thread, which is the field
behaving as designed rather than a missing value.

**AND THE MUTANT CORROBORATES §1.1 INDEPENDENTLY.** Both fire at
`rsp - base = 0x3FC0 = STACK_SIZE - 64`, the **freshly-seeded** position. The CI
artefact sat at `0x3A40`, 1,472 B lower — so that thread really had **run and
descended frames**, which §1.1 asserted from arithmetic alone and this measures
from the other side.

Revert returns `4bcbdf1bd1cea57f` at 1,319,306 B, **verified by rebuild, not
assumed**, warning-clean at `-Werror`.

### §7.1 — TWO MEASUREMENT DEFECTS IN MY OWN WORK, RECORDED NOT QUIETLY FIXED

**(a) The first build after editing `sched.c` produced a BIT-IDENTICAL kernel.**
`build/sched.o` (01:32) was older than `sched.c` (10:10) and `make image` did
**not** rebuild it. §INV.10 generalised — and it was caught **only because the
hash was checked**, not because `make` said anything. Forcing
`rm build/sched.o` then exposed **four compile errors** in the change (the new
comment block closed with `*/` early, orphaning the COST paragraph into code).
**Had the hash not been checked, this change would have been "verified" against
a binary that did not contain it** — and the mutants would have been too.

**(b) The first mutant run reported `SCHEDCHECK=0` for BOTH arms — and the
capture files did not exist.** `smoke-shell` hardcodes
`build/shell_serial.log` and ignores an external `SERIAL_LOG`, which is §INV.7's
shape and **DDR-1041's exactly** (three gates there "first reported no SMAP
marker … they set their own SERIAL_LOG, so the marker was in another file").
`grep -c` on a missing file returns `0`, so **a broken measurement read as a
clean result** — the DDR-1023 vacuous-capture class, and it would have been
reported as "the mutant did not fire", i.e. a false null on my own change.
Re-run with a **validity guard** that reports `MEASUREMENT-BROKEN` when the
capture is absent or carries no boot output, which is DDR-1092's rule that zero
must mean *the measurement broke*, never *the condition held*.

### §7.2 — THE NEGATIVE

The load-bearing half, because a false positive halts a CPU on the hottest path.
Clean kernel `4bcbdf1bd1cea57f`, hash pinned and re-verified after the last run
(DDR-1060 §9). Because `[schedcheck]` is `GLOBAL_FORBIDDEN` entry 75 of 77, a
single fire reddens whichever gate boots — so **`rc=0` on each gate IS the
zero-`[schedcheck]` claim**, and `smoke-shell`'s own capture is grepped directly
as positive evidence rather than inferred from `rc` (DDR-1041).

| gate | `-smp` | rc |
|---|---|---|
| `smoke-shell` | 1 | **0** |
| `smoke-smp` | 4 | **0** |
| `smoke-smppreempt` | 4 | **0** |
| `smoke-rqstress` | 4 | **0** |
| `smoke-blk-integrity` | 4 | **0** |

`smoke-shell`'s own capture greps to **`[schedcheck]` count = 0** — positive
evidence, not inferred from `rc` — and the same run reports **`global-forbidden
scan clean (77 patterns)`**, so the list is live at 77 and the entry was not
silently dropped. **Hygiene: ALL EIGHT PASSED.** Kernel hash **pinned and
re-verified identical before and after the last run**: `4bcbdf1bd1cea57f` →
`4bcbdf1bd1cea57f`.

Four of the five run at `-smp 4`, so **four idle threads per boot** are under the
check (DDR-1106's coverage), across single-CPU and 4-CPU boots including the
heaviest create/exit churn gate.

**Why the negative alone would not have been enough** (DDR-1106 §4's reasoning,
which applies unchanged): a clause wrong in the *forgiving* direction would skip
exactly as before and every gate would still pass. The negative rules out the
**strict** direction — where a false positive on the hottest path would redden
everything at once — and the four mutants rule out the forgiving one. **Neither
alone would do.**
