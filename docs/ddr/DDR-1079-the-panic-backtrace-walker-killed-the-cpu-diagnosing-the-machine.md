# DDR-1079 — the panic backtrace walker killed the CPU that was diagnosing the machine, and the scan that should have said so reported the symptom instead

**Status:** ACCEPTED
**Date:** 2026-09-06
**Artefact:** CI 34051826587, shard 4, `smoke-smplock`, tip `8e3ae50`.
**Two defects, one capture.** §2 is in the kernel; §5 is in the harness and is
why §2 took this long to see.

---

## 1. The artefact, and why it is trustworthy

Tip `8e3ae50` is a **docs + host-script commit** — `git diff --name-only
da33f15 8e3ae50` lists `ci.yml`, four Markdown files, the DDR, two new
`tools/ci/` scripts, `hygiene_check.sh` and a `Makefile` target that adds no
build input. **No kernel source.** The shard's own post-gate assertion printed
`kernel.bin: OK` against the build job's hash, so the binary cannot have changed
(the DDR-1009 class). The heartbeat:

```
[hb] t=500 … panics_silent=1 panic_stage=3 loser_cpu=1 loser_vec=13
     loser_rip=0xFFFFFFFF8000C3A6 … rqcpus=3 …
[blk] multi-inflight FAIL done=0x0000000000000000 spawned=2/2
[vblk] compl wait timeout unit=0 dest_cpu=1 dest_dticks=0 dest_abs=218
       bsp_abs=947 dest_present=1 …
[hb] t=1000 … rqcpus=2 …
[smp] blk integrity FAIL reference-read
```

**This is DDR-1019's instrument firing on a real failure for the first time with
a resolvable RIP and a vector.** Its own capture was a forced `ud2` mutant.

---

## 2. The mechanism, named from the artefact

`g_panic_loser_rip = r->rip` (`idt.c:855`) is the **faulting RIP from the
loser's trap frame**. Resolved against the exact binary (§INV.18):

```
0xFFFFFFFF8000C3A6  ->  isr_dispatch+0xE86
ffffffff8000c39f:  mov    -0x108(%rbp),%rax
ffffffff8000c3a6:  mov    0x8(%rax),%rdi      <-- FAULTED
ffffffff8000c3aa:  call   kputhex
```

That is `kputhex(bp[1])` in the **panic backtrace walker** (`idt.c:940-947`),
which read, in full:

```c
uint64_t *bp = (uint64_t *)r->rbp;
for (int i = 0; i < 8 && bp; i++) {
    kputs("  ");
    kputhex(bp[1]);              /* saved return address */
    kputs("\r\n");
    bp = (uint64_t *)bp[0];      /* previous frame */
}
```

`bp != 0` and nothing else. **`loser_vec=13` is the whole diagnosis**: a plain
load faulting with **#GP** rather than #PF means a **non-canonical address** — a
bad-but-canonical frame pointer would have been #PF/14. So `rbp` (or a link in
its chain) held garbage, and the walker dereferenced it.

### 2.1 The winner and the loser are the SAME CPU, and that is deducible

The RIP lies **after** `g_panic_stage = 3`, i.e. inside the printing path a CPU
reaches only by **winning** the CAS. Only one CPU can win. So CPU 1 won,
printed the banner, the exception line and the register dump, then faulted in
its own backtrace; the re-entry lost the CAS **to itself**, incremented
`g_panic_extra`, and executed `for(;;) cli; hlt`.

`panics_silent=1` is therefore not a second CPU. It is one CPU re-entering its
own panic. **This deduction does not rest on the banner's visibility** — see §5,
where that absence turns out to be an artefact of the reporting, not of the run.

### 2.2 The consequence is worse than a truncated report

The CPU that was diagnosing the machine **becomes a frozen CPU**. Everything
downstream in the capture follows from it and is measured, not inferred:
`dest_cpu=1` matches `loser_cpu=1`; CPU 1's ticks stop at `dest_abs=218` while
the BSP reaches 947 then 1347; `rqcpus` falls 3 → 2; unit 0's MSI-X-routed
completions strand into `compl wait timeout`, then `[blk] multi-inflight FAIL`,
then `[smp] blk integrity FAIL`.

**And the panic dump dies at exactly the point where it would have named the
original fault.** The register dump is printed *before* the backtrace, so it
survived; but the run reported a block-integrity failure, and the panic that
caused it was never mentioned (§5).

---

## 3. The fix, and it is not invented here

The **NMI walker eleven lines up in the same file** (`idt.c:638-651`) already
bounds every link, and its comment states the reason: *"Bound every link to the
16 KiB stack this frame is on (8-aligned, above rsp, within one STACK_SIZE) so a
garbage rbp cannot fault us in NMI context, where a #PF would be the end of the
report."*

That reasoning applies **with more force** on the panic path, where a fault is
the end of the report **and costs the machine a CPU**. The panic walker now
carries the same four checks — range `[rsp, rsp+16384)`, 8-byte alignment,
upward growth, and a ring-3 guard for the NMI walker's own stated reason (a
ring-3 frame's `rsp`/`rbp` are user values and the range bound proves nothing;
ring-3 faults do not reach this path today, `idt.c:703` routes them to
`sched_exit`, so the check costs nothing and removes an assumption).

**It also says why it stopped.** A truncated chain and a chain that ended
cleanly were otherwise the same two lines, and the difference is the whole
diagnosis (DDR-1049's rule that an absence must name itself):
`<frame chain ends: fp=0x…>`, with `fp` **printed, never dereferenced**.

---

## 4. Proof

**The pre-fix behaviour is measured by the CI capture itself**, which is
stronger than a synthetic mutant: vector 13 at that exact instruction, on a
binary whose hash the shard verified.

**The post-fix behaviour is measured on `smoke-mce`**, the one gate whose pass
condition *is* a panic (DDR-1044). Before this change **nothing anywhere
asserted the backtrace**. Two runs, both green, and the difference between them
is itself informative:

| run | frames | terminator | resolved chain |
|---|---|---|---|
| 1 | 6 | `<frame chain ends: fp=0x0>` | `sha256_final+0x87 <- chain_step+0xc1 <- aether_audit+0x103 <- aether_sectest+0xbd <- kmain+0x8ec <- kernel_entry.hang` |
| 2 | 2 | — | the MCE landed in the shallow `kmain`/hang context |

The 6-frame chain is **coherent and fully resolvable**, and it ended on
`fp == 0` — the natural end — **not on the bound**. That is the evidence that
the bound does not truncate real chains. The count varies because
`mce_inject.py` fires when the guest prints `PRADYOS_MCE`, so the CPU can be
anywhere; **arm G therefore asserts `>= 1`, not an exact count.**

**Arm G has two halves and the second is the one tied to the defect:** at least
one frame must print, **and `halting.` must follow it** — a walker that faults
mid-walk never reaches `kputs("halting.")`, it halts as a silent panic loser,
which is precisely what the CI capture did.

**M2** — the panic walker's bound made impossible (`hi = r->rsp`), **that walker
only**, kernel `96a9071ff2060bea` — fails arm G alone:
`[mce] FAIL: panic backtrace printed no frames`. A first attempt mutated *both*
walkers with one `sed`; it was discarded and redone against the panic walker
alone, because attribution from a mutation that changed two things is the
DDR-1042 failure mode. Reverting returns `973959192d113bd9` **bit-for-bit**.

`kernel.bin` **1,290,634 B — size UNCHANGED**, the additions fitting inside
existing page padding, so the CLAUDE.md size/headroom pair is untouched and
`ci-docstate-check` is unaffected. Verified by rebuild, not assumed.

---

## 5. The second defect: the scan reported the symptom and never looked for the cause

`check_global_forbidden` **returned at the first matching pattern**, and
`GLOBAL_FORBIDDEN` is roughly alphabetical. In this capture **three** patterns
match, and their positions decide what anyone ever sees:

| pattern | position | class |
|---|---|---|
| `blk integrity FAIL` | 21 | downstream symptom |
| `NEXUS KERNEL PANIC` | 28 | **cause** |
| `panic_stage=` | 29 | **cause** |

The scan matched position 21 and **returned before testing 28 or 29**. So a run
in which a CPU panicked was reported as a block-integrity failure, and the panic
was never named. Reproduced deterministically on a fixture carrying all three:
the shipped code reports one, the fixed code reports three.

**This is DDR-824's defect one level up.** That fix added 40 lines of context
because *"printing ONLY the matching lines threw away the diagnosis"*; printing
only the first **pattern** throws it away the same way when several match. And
it bears on the whole OPEN-2 investigation: captures ending in
`blk integrity FAIL` / `compl wait timeout` have been read as the primary event,
while a panic pattern may have been sitting unreported in the same file.

**Fix:** name every match, with up to three matching lines each. The detailed
block (matching lines + 40 lines of context) still goes to the **first** match,
unchanged, so nothing a reader already relies on is lost. **Deliberately NOT
re-ranked by a hand-written cause/symptom order** — that would be one more list
to keep in step with 76 patterns, and it would drift.

**This also explains the absence I nearly reasoned from.** `NEXUS KERNEL PANIC`
appears **zero** times in the 187 KB job log — but the job log carries only the
excerpt around the *reported* pattern, and the scan never tested that pattern.
The absence is evidence about the reporting, not about the run. §2.1's deduction
was re-derived without it.

---

## 6. A third thing, found by needing it: `sym_at.sh` could not run at all

§INV.18 and DDR-1019 both mandate resolving a RIP against its own binary, and
`tools/ci/sym_at.sh` is the tool for it. It used awk's **`strtonum()`, a GAWK
extension**, and this host's `/usr/bin/awk` is **mawk** — so every invocation
died with `function strtonum never defined`. **The one tool the procedure names
failed at the exact moment it was needed**, during a real CI panic
investigation; §2's resolution had to be done by hand.

Same class as the mawk defect already fixed once in `ci-probe-rodata-check`, and
the same lesson: a diagnostic proven only on the machine that wrote it is proven
only for that machine. Rewritten in `python3`, which is already a hard build
dependency (`fat-image`, `mce_inject.py`, `docstate_check.py`), so no new tool is
introduced. Checked both ways: it resolves `0xFFFFFFFF8000C3A6` to
`isr_dispatch+0xe86`, and returns rc=1 with `no symbol <=` for an address below
every symbol.

## 7. Not claimed

* **OPEN-2 IS NOT CLOSED, and this is not a claim about it.** The original
  exception CPU 1 panicked on is **still unknown** — the dump that would have
  named it is the thing that died. What changes is that the next occurrence can
  say: the walker survives its own backtrace, and the scan reports the panic
  instead of the block symptom.
* **No cause is named for the original panic**, and §NON-NEGOTIABLE 3 forbids
  guessing one. `loser_vec=13` is the vector of the **walker's** fault, not of
  the panic that preceded it.
* **This is not the `[apfreeze]` producer set being revised.** DDR-1019 named
  three producers; this is a fourth path to a frozen CPU, reached only from
  inside the panic path.
* **Nothing is fixed about garbage `rbp` itself.** A frame pointer was corrupt
  and this DDR does not say why. It stops that corruption from costing a CPU and
  the report.
* **No rate.** One occurrence.
* `GLOBAL_FORBIDDEN` **76, unchanged** — no pattern is added; the change is to
  how matches are reported. 178 gates unchanged (arm G is added to an existing
  gate, no new gate). No open issue moves (OPEN-1/12/13 untouched).
