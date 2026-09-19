# DDR-1105 — A validity check on `next->rsp` before `context_switch`

**Status:** SHIPPED. Implements DDR-1099 §7's named next step.
**Not a fix.** OPEN-2's corruption still has no named cause; this makes the next
occurrence say so *before* it costs a CPU, and catches the variants that produce
no exception at all.

---

## §1 — What DDR-1099 established, and what it deliberately did not build

DDR-1099 resolved the shard-7 `[apfreeze]` to a `#DB` raised by a `TF` bit that
`context_switch`'s `popf` restored out of the next thread's saved frame, and
narrowed it to one sentence: **`next->rsp` did not point at the frame
`context_switch` saved.** The evidence was two independent slots of one frame
holding values from elsewhere on that stack — `RFLAGS = 0x2702` (`TF|IF|DF`,
IOPL 2), which **neither legitimate writer can produce** (`sched_create` seeds the
constant `0x202`; `pushfq` captures a kernel thread's live flags, which have
neither TF nor DF and run at IOPL 0), and `R15` holding a `finish_task_switch`
return address when the only `call context_switch` in the kernel would have left
`0x…16497`.

Its §7 named this check and **refused to build it there**, for a stated reason: an
always-on check on the hottest path is the cost DDR-1047 refused for OPEN-2's sake.
§5 answers that on a measurement rather than by waving it away.

---

## §2 — The check, and the order is load-bearing

Immediately before `context_switch(&prev->rsp, next->rsp)` in `schedule_locked`:

1. **Skip** when `next->kstack_base == 0` (§3).
2. **8-aligned** — `next->rsp & 7`.
3. **In this thread's own kernel stack** — `kstack_base <= rsp` and
   `rsp + 64 <= kstack_base + STACK_SIZE`. The `+64` is the frame
   `context_switch` pops: `RFLAGS, r15, r14, r13, r12, rbp, rbx` plus the return
   address = **8 quadwords**, read straight off `context.asm:52-71`.
4. **Only then** dereference: the RFLAGS slot is at **`[rsp + 0]`**, because
   `pushfq` is the *last* push (`context.asm:59`), which is exactly why DDR-1099's
   faulting `popf` was the *first* restore instruction. Require
   `TF | DF | IOPL` clear.

**Steps 2 and 3 must precede step 4** — the whole point is that `next->rsp` may be
garbage, so bounds and alignment are what make the load in step 4 safe. Getting
this backwards would turn a detector into a second fault source, which is the
defect DDR-1079 fixed in the panic backtrace walker.

**§2.1 — `next != prev` is GUARANTEED here, and that is what makes reading
`next->rsp` meaningful at all.** `->rsp` is written only by `context_switch` as a
thread switches *away*, so on the *running* thread the field is stale by
construction and a check against it would be nonsense. It cannot happen: the
keep-running case returns earlier in `schedule_locked`, and the one path that can
substitute idle (`switch_wait_offcpu_sched` failing) re-tests and returns when
`next == prev` — the code says so in its own comment at `sched.c:1504`, *"next !=
prev here — the keep-running case returned above, so we never wait on
ourselves."* Checked by reading, not assumed, because the whole check rests on
it.

---

## §3 — The `kstack_base == 0` skip: measured, and a stated coverage limit

`init_idle` (`sched.c`) opens with `memset(idle, 0, sizeof(*idle))` and **never
assigns `kstack_base`** — the BSP idle is a static BSS `struct tcb` (`idle0`)
converted from the boot context, and AP idles are `kmalloc`'d in
`sched_ap_enter` through the same `init_idle`. **So every idle thread has
`kstack_base == 0`, and its `rsp` lives on a boot/AP stack outside any window this
check could derive.**

Without the skip the check would fire **on nearly every switch** — on a largely
idle system the idle thread is the commonest `next` — and would have reddened all
179 gates on the first boot. Recorded because it was caught *in design*, before the
first build, which is the DDR-1076 §5 discipline.

**The limit this buys is stated, not glossed:** a corrupt `rsp` on an *idle* tcb is
not covered. It is a narrow gap — an idle's `rsp` is only ever written by
`context_switch`'s own `mov [rdi], rsp`, never seeded — and DDR-1099's artefact was
not an idle (its `R15` held a `finish_task_switch` return address from a real
thread's frame). But it is a gap.

---

## §4 — The mask is `TF | DF | IOPL`, and each bit is justified separately

`0x100 (TF) | 0x400 (DF) | 0x3000 (IOPL)` = **`0x3500`**. DDR-1099's arithmetic:
`0x2702 - 0x202 = 0x2500`, bits 8, 10 and 13 — all three inside this mask.

- **TF** is set by a debugger. There is none here.
- **DF** — `grep -rnw std` over `arch/`, `kernel/` and `user/` (excluding
  `std*.h` spellings) returns **nothing**. DF is never set anywhere in this tree,
  and the SysV ABI requires it clear at every call boundary anyway.
- **IOPL** — kernel threads run at CPL 0 with IOPL 0; nothing writes `RFLAGS.IOPL`.

**Deliberately NOT masked:** the arithmetic flags (CF/PF/AF/ZF/SF/OF) and `IF`.
`pushfq` captures whatever the outgoing kernel thread's last arithmetic left, and
`IF` legitimately varies. Requiring `0x202` exactly would fire constantly.

---

## §5 — The cost, measured against the shape DDR-1047 actually refused

| | DDR-1047's refused instrument | this check |
|---|---|---|
| per what | **every `spin_lock` acquisition** (~1.9M per 5,000 ticks, measured) | every context switch |
| what | an **`rdtsc` PAIR** — partially serialising, tens of cycles each | 3 loads, 1 add, 5 compares, 5 branches |
| predictability | n/a | **never taken on a healthy boot** — perfectly predicted |

DDR-1047 refused *"an always-on `rdtsc` pair in `spin_lock`"* because it *"could
MOVE that bug rather than measure it"*. **A dozen predictable compares are not that
instrument**, and the nearest accepted precedent is DDR-1090, which added two loads
and a branch inside `yield()` — a hotter path still — and shipped.

**Opt-in is not the escape and is not taken.** DDR-1010 and DDR-1043 both
established that an opt-in instrument is guaranteed OFF in CI, which is the only
place OPEN-2 has ever appeared. A flag here would be a check that never runs where
it matters.

**And this is not exonerated in advance (DDR-1042).** It changes timing on the path
OPEN-2 lives in. If the signature changes after this lands, **this commit is a
candidate**, and "the diff is elsewhere" is not an argument.

---

## §6 — What it catches that today's behaviour does not

Today a corrupt frame is diagnosable **only when it happens to set `TF`** — the
`#DB` is what produced DDR-1099's readable report. DDR-1099 §6 named the other
case exactly, while arguing against masking TF: a frame that is equally wrong but
whose RFLAGS slot is benign yields *"an undiagnosable jump into nowhere"* — the
`ret` at `context_switch+0x1c` returns to a stale address with no exception, no
banner and nothing to resolve.

**Those are the cases this check converts into a named line**, and they are
presumably the commoner ones, since only a minority of garbage words have bit 8
set. That is the value: not a better report for the `#DB` case, but **a report at
all for the silent case.**

---

## §7 — On detection: name it, then halt, following this file's own precedent

`schedule_locked` runs **with `g_sched_lock` held and `IF` clear**, so there is no
unwinding to be done and no `panic()` is exposed to `sched.c`. It follows
`sched_exit`'s existing precedent for an unrecoverable scheduler state
(`cli` → `kputs("[panic] …")` → `for(;;) hlt`).

One `kline` emit (DDR-1055's rule — a composite built from several `kputs` calls
can be spliced by another CPU), carrying `tid`, `rsp`, `kstack_base` and the
RFLAGS slot, then halt. **Continuing is not an option**: continuing *is* the
undiagnosable jump.

`[schedcheck]` goes in `GLOBAL_FORBIDDEN` so it reddens whichever gate it lands
in — the precedent DDR-981 set with `[apfreeze]` and DDR-1049 with `panic_stage=`.

---

## §8 — Proof, and why there is no gate arm

**There is no gate arm and that is deliberate, not an omission.** The triggering
condition cannot be manufactured in the product — a kernel that corrupts
`next->rsp` on purpose is not a kernel any gate can run — and an arm asserting the
*absence* of a rare intermittent is unfalsifiable at any N this project can afford
(DDR-1082 costed exactly that shape and refused it). Proof is therefore a **forced
mutant on a recorded hash**, the DDR-1030 (`idle2=`) and DDR-1047 (lock dump)
standard for an instrument whose condition cannot be produced in product.

**Two mutants, landing on different clauses, each one line changed** (DDR-1042:
attribution from a mutation that changed two things is the failure mode). Clean
kernel `3933fa5f60ae607c`, 1,315,210 B.

**M1** — `sched_create`'s seed `0x202` -> `0x302`, i.e. TF set in the frame the
thread will `popf`. Kernel `684f0744ff60ba60`:

```
[schedcheck] next->rsp invalid tid=1 rsp=0x0000000007E57FC0 base=0x0000000007E54000 rflags=0x0000000000000302 halting.
```

`rflags` carries the mutation, so **clause 4** tripped; and `rsp - base = 0x3FC0
= 16320 = STACK_SIZE - 64`, 8-aligned and in bounds, so **clauses 2 and 3 did
not** — the dereference arm is live and the bounds arms are not what caught it.

**M2** — `t->rsp = ... sp` -> `... sp - 7`, i.e. the *pointer* is wrong while the
frame stays put, which is exactly the defect being modelled. Kernel
`c6b558900747e5b4`:

```
[schedcheck] next->rsp invalid tid=1 rsp=0x0000000007E57FB9 base=0x0000000007E54000 rflags=0x0000000000000000 halting.
```

`0x…FB9 & 7 == 1` trips **clause 2**; `rsp - base + 64 = 16377 < 16384` leaves
clause 3 untouched; and **`rflags` reads 0, still at its initialiser, so clause 4
was never reached.** That is §2's ordering claim MEASURED rather than argued —
the dereference really is guarded by the checks above it. **The two mutants are
told apart by the printed `rflags=` field itself**, so neither carries the other.

**The sentinel reddens the gate for free**, confirmed rather than assumed — both
mutant runs failed with `[smoke] FAIL — a probe reported '[schedcheck]' during
this gate's boot`, the DDR-1097 `[ringwalk]` precedent.

**THE LOAD-BEARING NEGATIVE, and it is the half that matters** — a false positive
here halts the machine on the hottest path. On the clean kernel: `smoke-shell`
(which also reports `global-forbidden scan clean (77 patterns)`, so the new entry
is live in the scan and not silently dropped), `smoke-smp`, `smoke-smppreempt`,
`smoke-rqstress` and `smoke-blk-integrity` — **all rc=0, all with zero
`schedcheck` lines**, covering single-CPU and 4-CPU boots including the heaviest
create/exit churn gate. Revert returns `3933fa5f60ae607c` BIT-FOR-BIT, verified
by rebuild rather than assumed.

---

## §9 — NOT CLAIMED

- **No fix.** OPEN-2's corruption has no named cause and this does not name one
  (§NON-NEGOTIABLE 3). It changes what the *next* occurrence can say.
- **OPEN-2 does not close**, and no rate is claimed.
- **TF is NOT masked before `popf`** — DDR-1099 §6 recorded that as the tempting
  wrong move and this does not make it.
- **Idle threads are not covered** (§3), stated as a limit.
- **No gate is added** (179 unchanged); §8 is why. `GLOBAL_FORBIDDEN` **76 ->
  77**, `'[schedcheck]'` inserted BEFORE the final list line so
  §NON-NEGOTIABLE 6's verification terminator did not move; that rule's stated
  count is updated in this same commit, as the rule itself requires. Verified
  **77, not 0**.
- **No defect in the scheduler is found or fixed, and none is alleged** — the
  kick path, the frame layout, `sched_create`'s seed and `context_switch` are all
  correct, and what is added is a check on a value that *should* never be wrong.
- **NOT exonerated in advance** (DDR-1042): this changes timing on the very path
  OPEN-2 lives in, so if the signature moves, this commit is a candidate. "The
  diff is elsewhere" is not an argument.
- **The `[schedcheck]` prefix has ONE producer today.** DDR-1097 recorded that
  `[ringwalk]` reached three producers and that a matched pattern then stops
  identifying its site; this line is self-identifying (it names `tid`, `rsp`,
  `base` and `rflags`) if that ever happens here.
- `kernel.bin` **1,311,114 -> 1,315,210 B**, the page-aligned 4,096 B, with the
  size/headroom pair recomputed in the same edit at all of its carriers.
