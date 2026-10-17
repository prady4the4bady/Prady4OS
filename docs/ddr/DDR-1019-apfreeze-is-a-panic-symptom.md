# DDR-1019 — `[apfreeze]` in the shard-9 capture is a PANIC SYMPTOM, not a scheduler defect

**Status:** MECHANISM NAMED from a real CI artefact, PROVEN by disassembly, and
reproduced locally. Instrument built and mutation-checked. **No fix to the panic
path attempted** — the mechanism explains the *signature*, not yet the exception
behind it, and §NON-NEGOTIABLE 3 forbids the rest.

---

## 1. The artefact

CI run 33323162053, shard 9, `smoke-blkmq-trace`, tip `6894062` (docs-only over
`8ad4012`). Serial, in order:

```
[hb] t=500  … panics_silent=1 …
[yieldstall] site=mnt_lock spins=158311 ticks=500 pid=43 cpu=1
[vblk] compl wait timeout unit=2 dest_cpu=3 dest_abs=262 bsp_abs=786  … lba=2400
[hb] t=1000 … panics_silent=1 …
[vblk] compl wait timeout unit=2 dest_cpu=3 dest_abs=262 bsp_abs=1286 … lba=2392
[hb] t=1500 … panics_silent=1 …
[apfreeze] cpu=3 ticks=262 rip=0xFFFFFFFF8000A2F8 cs=0x08 rflags=0x2402 if=0
           rsp=0x07C2A930 lvt=0x00020030 masked=0 svr=0x1FF swen=1 tpr=0
           isr48=0 irr48=1 pid=11 shot=1
           bt=0xFFFFFFFF8000027A,0xFFFFFFFF8000027A,0x07CE8000,0xFFFFFFFF80138D60
```

The job log reports `kernel 1106314B`. That kernel was **rebuilt bit-for-bit in a
worktree at `8ad4012`** and hashes `b0e4ccb83d4bb7ac` — so the addresses below are
resolved against the exact binary CI ran, which §INV.18 requires.

## 2. `rip=0xFFFFFFFF8000A2F8` is the panic-loser's halt loop

```
0xffffffff8000a2f6:  fa                cli
0xffffffff8000a2f7:  f4                hlt
0xffffffff8000a2f8:  e9 f9 ff ff ff    jmp 0xffffffff8000a2f6   <-- the frozen RIP
```

with a `lock add` on **`g_panic_extra`** immediately above it. `isr_dispatch`
spans `0xffffffff800099c0`–`0xffffffff8000a5e0`, so `0x8000A2F8` is
`isr_dispatch + 0x938` and it is **exactly** `idt.c:697`:

```c
if (!__atomic_compare_exchange_n(&g_panic_claimed, &expected, 1u, …)) {
    __atomic_add_fetch(&g_panic_extra, 1, __ATOMIC_RELAXED);
    for (;;) __asm__ volatile("cli; hlt");        /* <-- here */
}
```

**CPU 3 took a non-recoverable ring-0 exception, lost DDR-979's one-winner panic
latch, and halted itself with interrupts disabled — by design.** Everything
downstream follows: `if=0` with `irr48=1` (a timer pending and undeliverable),
ticks frozen at 262, its virtio-blk completions timing out (`dest_cpu=3`), and
`[apfreeze]` — a `GLOBAL_FORBIDDEN` entry — failing the gate.

**So this `[apfreeze]` is not a scheduler defect, an IF-masking defect, or
DDR-981 recurring. It is a CPU that panicked.** The backtrace agrees: it
terminates immediately at `isr_common.gs_kernel_in + 0x8` (twice), and its
remaining two words resolve to a stack address and to a location past
`__text_end` — neither is code, i.e. there is no caller chain to walk, which is
what an exception entry looks like.

## 3. The winner printed NOTHING, and that is the defect

`g_panic_extra` is incremented **only** on losing the CAS, so a winner existed.
Yet:

- `'NEXUS KERNEL PANIC'` **is** in `GLOBAL_FORBIDDEN` at `8ad4012` (73 patterns,
  appended by DDR-1009), and
- `smoke-blkmq-trace` **does** go through `boot_test.sh` (`Makefile:2173`),

so a printed banner would have killed the run at that line. Instead the boot ran
on for another ~1000 ticks and died on `[apfreeze]`. **No banner was printed.**

`g_panic_claimed` is a latch that is **claimed before the dump and never
released**, and losers are silent by design (DDR-979 §6 — deliberately, because
a "me too" line would reintroduce the interleaving that made OPEN-12's first
capture unreadable). The consequence was not stated there:

> **If the winner cannot complete its dump, the machine prints no panic output at
> all, every later panic is silenced, and the only visible symptom is CPUs frozen
> in the loser's halt loop.**

DDR-979 traded a garbled dump for a readable one. The failure mode it introduced
is **no dump**. That is why `[apfreeze]` has resisted scheduler-side
root-causing: on this path there is nothing scheduler-shaped to find.

## 4. Not DDR-1010's SWAPGS path

DDR-1010's continuous probe **is present in this kernel** (`syscall.c:111`,
called at `:138` before anything dereferences `current_thread`) and printed
**zero** `gs FAIL` lines in this capture. So the instrument built for that
hypothesis did its job: it excluded itself. This is a different path.

## 5. What is NOT established

- **Which exception, and on which CPU.** The winner's identity, vector and RIP
  are all unrecorded.
- **Why the winner stalled.** `console_panic_force_release()` runs *before* the
  CAS (`idt.c:673`), so the obvious console-lock deadlock is already handled; no
  further evidence exists.
- **That every past `[apfreeze]` is this.** DDR-1006's capture had a full
  `schedule <- sched_tick <- isr_dispatch` backtrace and DDR-1010's resolved
  through `sys_mmap`; neither RIP is this halt loop. **`[apfreeze]` is a symptom
  with at least three distinct producers, and matching on the sentinel name alone
  is the colour-matching DDR-975 §7 and DDR-1010 §2 each had to retract.**

Under §NON-NEGOTIABLE 3 that forbids a fix to the panic path. It does not forbid
making the next occurrence answer the question.

## 6. The instrument

Three additions to `kernel/idt.c`, none of which change the panic path's
behaviour:

- **`g_panic_stage`** — how far the winner got. `1` = claimed the latch and never
  reached the banner (this capture's case); `2` = banner out, so the console
  works; `3` = exception identified.
- **`g_panic_loser_{cpu,vec,rip}`** — what the **first** loser was, *recorded,
  not printed*. Printing from a loser would reintroduce exactly the interleaving
  DDR-979 removed. First-writer-wins via an exchange, so a third CPU cannot
  overwrite what the heartbeat will report. `lapic_id()` rather than
  `this_cpu()`, per DDR-981 §9 — a ring-0 fault can arrive with a GS base that is
  not this CPU's, and that is itself worth being able to see.
- The heartbeat prints all four **inside the existing `if (g_panic_extra)`
  block**, so a healthy boot emits not one extra byte.

## 7. Measured — and the CI signature reproduced locally

Baseline kernel **`6836dc723f31fc3e`**, `-Werror` clean, 1,126,794 B.

**M-B (non-vacuity).** An AP is forced down the loser branch: pre-claim the latch
and execute `ud2` from `sched_tick` when `!is_bsp && g_ticks > 300`. Mutant
kernel `640fdd2c17451143`, at `-smp 4`:

```
[hb] … panics_silent=1 panic_stage=0 loser_cpu=3 loser_vec=6 loser_rip=0xFFFFFFFF800122D4
[apfreeze] cpu=3 ticks=250 rip=0xFFFFFFFF8000A5B0 … if=0
NEXUS KERNEL PANIC: 0 occurrences
```

`loser_vec=6` is `#UD`, what was injected. `loser_rip` resolves to
`sched_tick + 0x74`, the injected site. `0xFFFFFFFF8000A5B0` is
`isr_dispatch + 0x990` — the halt loop **in this binary** (a different offset from
the CI kernel's `+0x938`, which is the point of §INV.18).

**This is a faithful local reproduction of the CI capture's shape**:
`panics_silent=1`, no banner, one CPU frozen in the halt loop, block completions
stranded on it, `[apfreeze]` failing the gate. On the CI kernel that state was
mute; on this one it names the loser and says `stage=0`.

Two earlier M-B attempts failed and are recorded because they are reusable
knowledge: injecting on the **BSP** halts the machine before any heartbeat prints
(no output at all), and `*(volatile uint64_t *)0x8 = 0` **does not fault** —
page 0 is mapped writable, so the store simply succeeded. `ud2` is the reliable
ring-0 injector here.

Gate suite on the baseline, one hash verified before and after each run:
`smoke-blkmq-trace` (**the gate that failed in CI**), `smoke-blk-integrity`,
`smoke-shell` (73-pattern scan clean), `smoke-blkmq` — all PASS.
`hygiene_check.sh` ALL THREE PASSED.

## 8. What the next occurrence will say, and what to do with it

- `panic_stage=1` → the winner claimed the latch and died before the banner.
  That is this capture again, and the next question is what stops a CPU between
  `idt.c:699` and `:701` — the only code there is `kputs`.
- `panic_stage>=2` with no banner in the log → the banner was emitted and lost,
  which is a capture problem, not a kernel one.
- `loser_vec` names the second exception. If it is 14 (`#PF`) or 13 (`#GP`), it
  is in the family OPEN-12 and DDR-985 have been chasing.
- `[apfreeze]` whose RIP is **not** the halt loop is a different producer
  entirely — resolve it against the binary before reading it as this.

**A liveness fix for the latch is the obvious next change and is deliberately NOT
made here.** A watchdog that lets a second CPU take over the dump after N ticks
would have produced a dump in this capture — but it also re-opens the
interleaving DDR-979 closed, and redesigning the panic path on one artefact, days
from a release, is how the garbled-dump problem got created in the first place.
Named, measured, and left for a decision.
