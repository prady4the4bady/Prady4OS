# DDR-979 — Intermittent ring-0 panic in early boot (OPEN), and the capture bug that hid it

Status: OPEN defect, recorded with the evidence that exists. **No fix** — the one
artefact was destroyed at capture time, and §NON-NEGOTIABLE 3 forbids guessing.
A harness fix ships here so the next occurrence is diagnosable.
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§INV.4).

---

## 1. The event

CI run `32595646699`, shard 0, head `b43d6b0`, first gate of the shard:

```text
SYSCLOSE OK

*** NEXUS KERNEL PANIC ***
component: NEXUS isr
```

`component: NEXUS isr` is `idt.c:463` — a **ring-0** exception. Everything before
it is a normal boot: `[boot-stamp] A … t=181`, HELLO/WXVIOL/SYSTEST/INPUTTST
loaded, all the SYS* self-tests OK. The panic lands at **t≈185**.

`shard 0: FAILED at smoke-blk-integrity after 1 of 22 gates`.

## 2. It is not from the diff it failed on

Three independent reasons:

1. **`b43d6b0`'s only kernel change is gated off at that point.** It adds
   `cputicks[…]` to the heartbeat, inside `if ((now % 500) == 0)`. The panic is
   at t≈185 and **the log contains no `[hb]` line at all** — the first heartbeat
   would be t=500. That code never executed.
2. **Same SHA, one red and one green.** `b43d6b0` ran two independent full
   matrices: run `32595644683` (push) **succeeded**, run `32595646699`
   (pull_request) **failed**. Same commit, same binary.
3. **0/10 locally** at branch HEAD (`smoke-blk-integrity`, ten consecutive runs,
   zero panics).

So: a pre-existing intermittent, not a regression. It is nonetheless a **real
ring-0 panic** and it has not been recorded before, so it is logged here rather
than absorbed as noise.

## 3. Why it cannot be diagnosed yet — the capture bug

The panic prints its identifying block right after the banner:

```c
kputs("exception: "); kputs(name);
kputs("  vector="); kputhex(r->vector);
kputs("  error="); kputhex(r->err_code);
dump_line("RIP=", r->rip);   /* … CS, RFLAGS, RSP, CR2, GPRs */
```

None of it survived. What the job log holds is:

```text
component: NEXUS isr
make: *** [Makefile:2307: smoke-blk-integrity] Error 1
exception: make: Leaving directory '/home/runner/work/Prady4OS/Prady4OS'
```

`exception:` is guest serial on **stdout**; `make: ***` is make's **stderr**.
Two separate file descriptions writing one pipe with no ordering guarantee, so
they interleave — here **mid-line**, overwriting the exception name, vector,
error code and RIP. The only lines that identify the fault are exactly the lines
that were lost.

**Fix (shipped here):** `tools/ci/run_shard.sh` now runs `make … 2>&1`, putting
both streams on one file description so writes are ordered by the kernel. Costs
nothing; makes the next occurrence readable.

This is the same family as the vacuous gates this project keeps finding
(DDR-971, DDR-973 §6, DDR-978 §3.2, DDR-880's harness-echo detector) — not a
test that asserts too little, but a harness that destroys the evidence it
collected. Worth naming separately because the failure mode is invisible until
you need the artefact.

## 4. What is known, and what a diagnosis needs

**Known:** ring-0 exception; ~t=185; after `SYSCLOSE OK` and the INPUTTST.ELF
load; on shard 0's first gate; rate low enough that 10 local runs and one of two
CI matrices on the same SHA saw nothing.

**Needed:** one capture with the `exception:`/`vector=`/`RIP=` block intact. With
§3's fix the next occurrence supplies it; `tools/ci/sym_at.sh` then maps the RIP
to a symbol.

**Do not guess from `component: NEXUS isr` alone.** It is printed for *every*
non-recoverable ring-0 vector — #GP, #PF, #UD, #DF all land there. Nothing in the
surviving text distinguishes them, and picking one would be colour-matching.

## 5. Tracking

Added to `CLAUDE.md` §OPEN ISSUES as **OPEN-12**, and to `docs/build_status.md`.
Reopen-on-sight: any `*** NEXUS KERNEL PANIC ***` in CI or a local run. The
PR-#6 branch is unaffected — its head is green and this does not gate it.

---

## OPEN-12 — the readable artefact, at last (2026-08-23)

`run_shard.sh`'s `2>&1` merge worked. CI run 32607127492, shard 1, head
`44ccce8`, gate `smoke-rqstress-liveness` at `QEMU_SMP=4`. The block this DDR
was written to capture:

```text
*** NEXUS KERNEL PANIC ***
component: NEXUS isr
exception: #GP general protection  vector=0x0D  error=0x0
RIP=0x0000FFFFFFFF8001      CS=0x8      RFLAGS=0x00044287
RSP=0x0000000007C2AECA      RBP=0x00000000000007C2
RBX=0x20C7000000000000      R12=0xAF38000000000000
R14=0xAEBE000000000000      R15=0x2646000000000000
backtrace:
  0x0000000000000000
halting.
```

### 1. Read the dump before believing it — it is 2 bytes out

The obvious move is to diagnose from these registers. **Do not.** They are
internally inconsistent, and the inconsistency is systematic, not random:

| check | result |
|---|---|
| `RSP >> 16` | `0x7C2` — **exactly** the printed `RBP` |
| `RIP << 16` | `0xFFFFFFFF80010000` — a plausible kernel text address; the printed `RIP` is not |
| `RSP % 8` | **2** — a ring-0 exception frame cannot be 2-byte aligned |
| low 48 bits zero | `RBX`, `R12`, `R14`, `R15` — the left-shifted counterpart |

Every field is a splice of two adjacent qwords. The frame is being read at an
address **2 bytes off**, so each printed 64-bit value is the tail of one slot
concatenated with the head of the next. `RBP == RSP >> 16` holding *exactly* is
the proof: those are adjacent members of `struct regs`.

**Consequence: no register value in this dump names anything.** A session that
resolves the printed `RIP`, or reasons from `RFLAGS` (which decodes as
`NT=1 AC=1` — both individually alarming, and both meaningless if spliced), is
reading noise. That is the trap this addendum exists to close, and it is the
same class as §NON-NEGOTIABLE 18: a value that looks like an address is not one.

### 2. What is actually established

- The exception is **#GP, vector 0x0D, error code 0**. The vector and mnemonic
  come from `r->vector`, which the dispatcher has before it walks the frame, so
  those two fields survive the misalignment.
- It is on an **AP** (`cs=0x8`, ring 0), and it **halts** that CPU.
- **Two panics occurred in the boot**, not one — the captured window shows the
  tail of a first dump (`backtrace: / halting.`) immediately before the second.

### 3. `[apfreeze]` here is a CONSEQUENCE, not a second defect

The same log carries four `[apfreeze] cpu=1 ticks=183 … if=0` lines, and the
first reading — mine — was that an AP had frozen with the DDR-981 fix in place,
i.e. a B#3 regression. **That is wrong.** Resolving the reported
`rip=0xFFFFFFFF80009BC7` against the **`BSP_LIVENESS=1`** build the gate
actually runs (not the default build — the addresses differ, `timer_tick` moves
`0x9be0` → `0x9c10`) puts it inside `isr_dispatch`, in the panic handler's own
frame-pointer print loop: `kputhex` / `kputs` walking `-0xc8(%rbp)`.

So the sequence is: **AP takes a #GP → panics → prints → halts → its `pc->ticks`
stop → the BSP's freeze detector NMIs it and reports a frozen CPU.** The
detector is doing its job — an AP whose ticks stopped *is* stopped — but
`[apfreeze]` is downstream of the panic here, and `if=0` is simply normal ISR
context, not the DDR-981 masked-yield livelock.

**Do not read an `[apfreeze]` line as a B#3 recurrence without first checking
the log for a panic above it.** DDR-981 §7 names "the first `[apfreeze]` line in
CI" as B#3's reopen condition; this refines that: the reopen condition is an
`[apfreeze]` with **no preceding kernel panic**, whose RIP resolves into a spin
or scheduler path rather than into `isr_dispatch`.

### 4. Next instrument — fix the dumper before diagnosing the dump

The misalignment must be resolved first, because until it is, every future
OPEN-12 capture is equally unreadable. Two candidates, and they are
distinguishable:

1. **The dumper's frame pointer is wrong** — it reads `struct regs` from a
   miscomputed address, and the real exception frame is fine. Then the #GP is a
   separate matter and the dump has been lying in every previous capture too.
2. **The exception frame really is misaligned** — the stack was 2 bytes out
   when the exception was taken, which would also be a strong candidate for
   *causing* a #GP.

Discriminator: have the panic path print `(uint64_t)r` itself and
`r->vector`/`r->err_code` (known-good fields) alongside `%rsp % 16`. If `r` is
8-aligned while the printed `RSP` is not, it is candidate 2.

### 5. CORRECTION to §1 — the frame is probably fine; the CONSOLE is unlocked

§1 concluded "the frame is being read 2 bytes off". That is not established, and
a simpler explanation fits the same evidence better. Recording the correction
because the two readings lead to completely different fixes.

**The dumper cannot independently be misaligned.** `dump_line` prints `r->rip`,
`r->rsp` … — plain struct member reads off the `r` the dispatcher was handed.
There is no pointer arithmetic to get wrong. So §1's reading requires `r` itself
to be misaligned, i.e. a genuine stack bug.

**But the panic path prints with no console lock, by design.** DDR-970 added
`console_line_force_release()` immediately before the dump, for a good reason
quoted in the code: a ring-0 fault skips the ring-3 trylock branch, so a CPU
that was mid-`[hb]` still holds `g_line_lock` and is about to halt forever
holding it, which would turn a diagnosable panic into a machine-wide hang.

The consequence was not carried forward: **from that point the dump is
completely unserialised**, and `dump_line` is three separate unlocked calls
(`kputs` label, `kputhex` value, `kputs` newline). §2 established that **two
panics occurred in this boot**. Two CPUs in `kputhex` at once interleave
character by character, and hex digits are exactly 4 bits each — so four
interleaved digits *are* a 16-bit shift. `RIP=0x0000FFFFFFFF8001` is 4 digits
from one stream followed by 12 from the other, and `RBP == RSP >> 16` is the
same splice seen twice.

**Both hypotheses predict a 16-bit shift**, which is why §1 looked convincing.
The console one additionally explains why the log shows one dump's tail
(`backtrace: / halting.`) immediately before the other's header, and it needs no
new defect — only a consequence of a fix already in the tree.

§1's *operational* conclusion stands unchanged and is the important part: **no
register value in that dump names anything.** What changes is the repair.

### 6. The fix — one printer, not one lock

Re-locking the panic path would reintroduce exactly the deadlock DDR-970
removed. Instead, let only the **first** CPU to panic print:

- claim with an atomic CAS on a `g_panic_claimed` latch;
- the winner prints the full dump as today, still after
  `console_line_force_release()`, so DDR-970's property is untouched;
- losers print **nothing** and halt — a one-line "me too" marker would
  interleave with the winner's dump and reintroduce the problem it is meant to
  avoid;
- so that a second panic is not silently lost, losers bump a counter surfaced as
  one fixed-width numeric on the existing `[hb]` line (the same justification
  `ymask` carries: a global read on a line already printed once per 500 ticks).

This makes every future OPEN-12 capture readable, which is the precondition for
diagnosing it at all — and it is why no fix for the #GP itself is attempted in
this DDR. The vector (0x0D) and error code (0) remain the only trustworthy
fields, and they come from `r->vector`/`r->err_code`, printed before the
interleave has much chance to start.
