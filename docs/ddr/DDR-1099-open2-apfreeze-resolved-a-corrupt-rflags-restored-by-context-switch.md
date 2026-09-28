# DDR-1099 — THE OPEN-2 `[apfreeze]` RESOLVES TO A `#DB` FROM A CORRUPT RFLAGS RESTORED BY `context_switch`

Status: **ARTEFACT CAPTURED AND RESOLVED. NO FIX. NO CAUSE NAMED FOR THE
CORRUPTION. OPEN-2 DOES NOT CLOSE.**
Date: 2026-09-12. Docs only — no code change; `kernel.bin` untouched.

## §0 — WHAT THIS IS

CI **34666584466**, shard 7, `smoke-smpsched`, head **`b73d013`** — a
**documentation-only commit** whose own post-gate step printed `kernel.bin: OK`,
so the binary is the one that had already gone **2/2 green on `709d0e2`**. The
DDR-1009 class: one binary, two outcomes.

This is the **first `[apfreeze]` since DDR-1062's 42-suite window** (which
recorded zero and bounded the per-suite rate below 6.9%). **One occurrence is not
a rate and no rate is claimed.** It is also the first `[apfreeze]` in this
project's history to arrive with **a named exception and a faulting instruction**.

§NON-NEGOTIABLE 3 is satisfied for the *proximate* mechanism and **not** for the
origin, so **no fix is made, proposed or designed.**

## §1 — DDR-1088's FIX DELIVERED, AND THAT IS WHY THIS WAS READABLE

DDR-1088's whole finding was that **no CI job log had ever printed a frame of the
panic report**: the scan showed matching lines plus 40 lines of *leading* context
for the *first pattern in list order*, and a panic is written **summary-first**,
so for the one pattern whose matching line carries no information the printer
showed that line and stopped.

This job log carries **the entire report**, because DDR-1088 made the scan name
every match and print context in both directions:

```
matched: NEXUS KERNEL PANIC
  *** NEXUS KERNEL PANIC ***
  | component: NEXUS isr
  | exception: #DB debug  vector=0x0000000000000001  error=0x0000000000000000
  | RIP=0xFFFFFFFF800002C4   CS=0x8   RFLAGS=0x0000000000002702
  | RSP=0x0000000007C2A6E8   RBP=0x0000000007F9FAF8   R15=0xFFFFFFFF800160DD
  | backtrace:
  |   <frame chain ends: fp=0x0000000007F9FAF8>
  |   <no frames: rbp unusable>
  | halting.
```

DDR-1079's bounded walker also did its job: `RBP = 0x07F9FAF8` is outside
`[RSP, RSP+16384)`, so it **declined to dereference it and said so** instead of
faulting mid-report and costing the machine a second CPU.

## §2 — §INV.18 FIRST: EVERY ADDRESS RESOLVED AGAINST ITS OWN BINARY

`b73d013` rebuilds to **`0693e5b04685ad60`, 1,311,114 B — bit-identical to the
CI binary**, verified by rebuild in a checkout of that exact commit, not assumed.

| address | resolves to |
|---|---|
| `[apfreeze]` `rip=0xFFFFFFFF8000C8F7` | `isr_dispatch+0xfe7` |
| panic `RIP=0xFFFFFFFF800002C4` | **`context_switch+0x14`** |
| `R15=0xFFFFFFFF800160DD` | **`finish_task_switch+0xd`** |

**The `[apfreeze]` RIP is NOT DDR-1019's producer, and the offset alone would
have said it was.** DDR-1088's shard-3 freeze resolved to `isr_dispatch+0xfe7`
too — *in a different binary* (`81b379053094041c`), which is precisely why
§INV.18 says an address does not identify a binary. Disassembled here, it is the
`jmp` of a `cli; hlt` loop entered **after printing `halting.`**:

```
c8e9:  mov $<"halting.\r\n">,%rdi ; call kputs
c8f5:  cli
c8f6:  hlt
c8f7:  jmp c8f5          <- the frozen RIP
```

So it is the **winner's terminal halt at the end of a completed report**, not
DDR-1019's *losing* branch of the panic latch. `panics_silent=0` agrees: nothing
lost the CAS. `panic_stage=3` ("exception identified", `idt.c:941`) agrees too.
**A fifth distinguishable `[apfreeze]` producer**, and the only one so far that
is a *consequence of a diagnosed panic* rather than an undiagnosed wedge.

## §3 — THE PROXIMATE MECHANISM IS INSTRUCTION-EXACT

`context_switch` (`arch/x86_64/context.asm`) restores with:

```
2c1:  popf            <- restores the INCOMING thread's saved RFLAGS
2c2:  pop %r15
2c4:  pop %r14        <- the panic RIP
```

`RFLAGS = 0x2702` decodes as **`TF | IF | DF`, `IOPL = 2`**, reserved bit 1 set.

`popf` at `2c1` set **TF**. The CPU then executed `pop %r15` at `2c2` and, TF
being set, raised a single-step **`#DB`** on completing it — with the trap
frame's RIP pointing at the *next* instruction, `2c4`. **That is exactly the
panic RIP.** The kernel has no recovering `#DB` handler, so `isr_dispatch`
panicked, the CPU printed its report and halted.

Everything downstream is measured, not inferred: CPU 2's ticks stop at **157**
while the BSP runs on to **4679**; `[vblk] compl wait timeout unit=1 **dest_cpu=2**
dest_abs=157` repeats for the rest of the boot; the gate times out.

## §4 — THE RFLAGS COULD NOT HAVE COME FROM EITHER LEGITIMATE WRITER

There are exactly two writers of that stack slot, and **neither can produce
`0x2702`**:

1. **`sched_create` seeds a constant.** `sched.c:1156` — `*--sp = 0x202;`
   */\* rflags: IF | reserved bit 1 \*/*. TF, DF and IOPL are never seeded.
2. **`context_switch`'s own `pushfq`**, which captures the live flags of a kernel
   thread being switched out. A kernel thread running normally has neither TF nor
   DF set and runs at IOPL 0.

`0x2702 - 0x202 = 0x2500` — bits 8 (TF), 10 (DF) and 13 (IOPL high). **So the
value popped into RFLAGS was not written by either writer of the RFLAGS slot.**

## §5 — AND R15 CONFIRMS IT INDEPENDENTLY: THE FRAME IS NOT THE ONE THAT WAS SAVED

`R15 = finish_task_switch+0xd`. Disassembled, `finish_task_switch+0x8` is
`call this_cpu`, a five-byte instruction ending at **`+0xd`** — so that value is
**the return address of a `call this_cpu`**, a datum that only ever lives on a
stack.

And it cannot be this frame's return address either: **the only
`call context_switch` in the whole kernel is at `0xffffffff80016492`**, so a
legitimate saved frame's return-address slot holds `0xffffffff80016497`.

Two independent slots of one frame therefore hold values from **somewhere else on
that stack** — the leftovers of a previous `finish_task_switch` activation, which
does `push rbp; mov rsp,rbp; sub $0x20,rsp; call this_cpu` and leaves exactly
such a return address and such locals behind.

**THE NARROWING: `next->rsp` did not point at the frame `context_switch` saved.**
Stale, recycled or misaligned — the artefact does not say which, and this DDR
does not guess.

## §6 — WHAT IS NOT CLAIMED, AND THE ONE TEMPTING WRONG MOVE

* **NO FIX, NO CAUSE.** What corrupted `next->rsp` is unknown. §NON-NEGOTIABLE 3
  forbids a fix on a narrowing, and none is made, proposed or designed.
* **NOT ATTRIBUTED TO DDR-1096 §3, although it is the same family.** That section
  hypothesised an unlocked all-threads ring walk in the timer ISR reaching a
  reissued TCB, and DDR-1097 built `OPEN2_HUNT`'s `tid` re-check for exactly a
  recycled object. A stale `next->rsp` is *consistent* with that — and **a
  matching mechanism is not an attribution (DDR-1056)**. The hunt has still never
  fired, and nothing here makes it have fired.
* **NOT ATTRIBUTED TO, NOR EXONERATING, `b73d013` or `709d0e2`.** The binary is
  bit-identical to one already twice green, which is evidence about *this commit's
  content* and not an exoneration — "the diff is elsewhere" is not an argument
  (DDR-1042).
* **NO RATE.** One occurrence. DDR-1062's <6.9% bound was computed over a window
  containing zero; this is one, outside that window, and a bound is not updated
  from a single observation.
* **`[apfreeze]` NOW HAS FIVE KNOWN PRODUCERS.** Before reading any future one as
  this, resolve its RIP against its own binary — this DDR is itself the case
  where the *offset* matched a previously-recorded producer in a *different*
  binary and the instruction did not.
* **THE TEMPTING WRONG MOVE, recorded so it is not made:** masking TF in
  `context_switch`'s restore (`and $~0x100` before `popf`) would stop the `#DB`
  and **would not fix anything** — the frame would still be the wrong frame, the
  `ret` at `2cc` would still return to a stale address, and the failure would
  move from a diagnosed panic with a full report to an undiagnosable jump into
  nowhere. It would delete the best artefact OPEN-2 has produced. **Do not.**

## §7 — WHAT THE NEXT OCCURRENCE SHOULD CARRY

Recorded rather than built, because an always-on check on the hottest path in the
kernel is exactly the cost DDR-1047 refused for OPEN-2's sake (an instrument on
this path can move the bug rather than measure it), and because its own vacuity
analysis and mutant have not been done:

A validity check on `next->rsp` in `schedule()` *before* the call — inside the
thread's own kernel-stack bounds, 8-aligned, and with
`*(uint64_t *)next->rsp & (TF|DF|IOPL)` clear — would turn the next occurrence
from "a `#DB` one instruction after `popf`" into "the frame was already wrong,
here is the thread and the value". **That is a design note, not a decision**: it
needs its own DDR, its cost measured, and a forced mutant, exactly as DDR-1047
and DDR-1093 required of their instruments.
