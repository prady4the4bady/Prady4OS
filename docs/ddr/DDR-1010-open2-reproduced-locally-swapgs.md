# DDR-1010 — OPEN-2 reproduced locally, with a cause: a broken SWAPGS discipline, not a scheduler defect

**Status:** ARTEFACT + DETECTOR. Root cause located to a *mechanism and a path*;
the defect in the source is NOT yet named, and no fix is attempted.
**Supersedes** the reading DDR-1006 §7 pre-registered for a null campaign.

---

## 1. The campaign DDR-1006 prescribed returned a clean null — and it was the wrong gate

DDR-1006 §7 said: reproduce with `smoke-smppreempt` at N=20 on kernel
`bb9c6187a30bb0dd`. Done, and completed:

```
20 runs, 20 PASS, one kernel hash bb9c6187a30bb0dd, zero [apfreeze]
```

`[apfreeze]` is `GLOBAL_FORBIDDEN` entry 2 and `smoke-smppreempt` runs through
`boot_test.sh` (`Makefile:2117-2121`), so **PASS implies the sentinel was absent**
— that conclusion does not depend on the per-run serial snapshots, which is
fortunate, because only 3 of the 20 were kept (see §6).

DDR-1006 §7 pre-registered what to conclude: *"If it does not reproduce in 20
local boots … then this is CI-only like OPEN-1 route 1."* **That conclusion is
now known to be wrong**, on two counts.

### 1.1 N=20 was underpowered for the rate CI actually showed

`smoke-smppreempt` runs once per suite (shard 4). Pooling DDR-1009's twelve
suite-runs on this kernel, CI ran that gate ~12 times and saw the freeze once:
**p ≈ 0.08**. Then `P(0 in 20 | p=0.08) = 0.92²⁰ ≈ 0.19` — a **19% chance of
seeing exactly this null even if the local rate equals CI's**. Twenty clean runs
were never going to settle it; 95% power against p=0.08 needs
`n ≥ ln(0.05)/ln(0.92) ≈ 36`.

The arithmetic was not done before the campaign was specified. It should have
been — this project has paid for an underpowered experiment once already
(DDR-1002, effective N≈1).

### 1.2 It reproduces locally — on a DIFFERENT gate

While running post-change hygiene, **`smoke-blk-integrity` failed on the first
attempt**, kernel `29c792a8b8f3b056`. Same freeze site as DDR-1006's CI capture,
`rip=0xFFFFFFFF8000A4FE`:

```
[apfreeze] cpu=1 ticks=186 rip=0xFFFFFFFF8000A4FE if=0 rsp=0x07D03BC0
           lvt=0x20030 masked=0 svr=0x1FF swen=1 tpr=0 isr48=0 irr48=1 pid=22
           bt=0xFFFFFFFF8000027A,0xFFFFFFFF8000DF9A,0xFFFFFFFF8000E137,0xFFFFFFFF80019606
```

So the gate to reproduce on was never `smoke-smppreempt`. Twenty runs of it
measured the wrong thing well.

## 2. The backtrace, resolved — and it is not DDR-1006's

| address | symbol |
|---|---|
| `0xFFFFFFFF8000A4FE` (RIP) | `isr_dispatch` |
| `0xFFFFFFFF8000027A` | `isr_common.gs_kernel_in` |
| `0xFFFFFFFF8000DF9A` | `map_core` |
| `0xFFFFFFFF8000E137` | `vmm_map_in` |
| `0xFFFFFFFF80019606` | `sys_mmap` |

**A ring-3 `mmap` syscall**, into `vmm_map_in` → `map_core`, took an exception,
and the CPU wedged in `isr_dispatch` with `if=0`, ticks frozen at 186 while the
BSP reached 17500.

DDR-1006's CI capture reached the *same RIP* from
`smp_ap_entry → isr_common.gs_kernel_in → isr_dispatch → sched_tick → schedule`
— an AP in its timer ISR. **Two unrelated callers, one wedge point.** That is
evidence about `isr_dispatch` itself, and it retires the framing of OPEN-2 as a
scheduler or timer defect: the timer path was one way in, not the cause.

## 3. The primary event: GS is wrong on a ring-3 syscall entry

Four lines before the freeze, in order:

```
[percpu] gs FAIL (syscall ctx)
[percpu] current FAIL (syscall ctx)
[fd] write EBADF pid=4026597203 fd=1
*** NEXUS KERNEL PANIC ***
component: NEXUS isr
exception: #GP general protection  vector=0x0D  error=0x00
RIP=0xFFFFFFFF8000E38C   RAX=0x0000FF53F000F000   RDI=0x0000FF53F000F000
```

`syscall.c:135-147` is the DDR-SMP-3a probe, one-shot on the first `sys_getpid`:
it reads `%gs:0` from a syscall entered at ring 3 and checks `pc->self == pc`.
Its own comment: *"this only works when the SWAPGS discipline is balanced."*
**It printed FAIL.**

`pid=4026597203` is `0xF000F053`, and `RAX`/`RDI` are `0x0000FF53F000F000` —
`0xF000` is the BIOS segment. So `current_thread`, resolved through the bad GS
base, pointed into ROM.

The chain is then mechanical, and every link is in the capture:

1. SWAPGS discipline breaks on one CPU at a ring-3 syscall entry → GS base garbage.
2. `current_thread` resolves into ROM → `[percpu] current FAIL`, garbage pid.
3. A later `sys_mmap` → `vmm_map_in` → `map_core` dereferences through it → `#GP`.
4. The exception is taken and the CPU wedges in `isr_dispatch`, `if=0`, ticks frozen.
5. That CPU's MSI-X block completions are never serviced →
   `[vblk] compl wait timeout unit=1 dest_cpu=2 dest_abs=184 bsp_abs=11330
   ticks[11330,186,184,11279]` → `[smp] blk integrity FAIL reference-read`.

Step 5 is DDR-977 §8.2's chain exactly. **The block layer and the scheduler are
both innocent, again, and this time the first domino is visible.**

### 3.1 The GS base was ZERO — the read landed on the real-mode IVT

This is worth pinning down, because "GS is wrong" and "GS is the USER's" are
different defects with different fixes.

`this_cpu()` is one instruction, `mov %gs:0, %rax` (`percpu.c:113`). The panic
block reports `RAX = RDI = 0x0000FF53F000F000`. With a **GS base of 0** that read
takes linear address 0 — which in the identity map is the **real-mode interrupt
vector table**, an array of `offset:segment` pairs. `F000:F053` is a BIOS entry
point, and `0x0000FF53F000F000` is exactly two such vectors packed into a
quadword.

So `this_cpu()` did not return a stale or randomly corrupted pointer. **It
returned the IVT**, which means the GS base was 0 — i.e. the **user's** GS base
was still active while executing a syscall in ring 0.

That is precisely the state `percpu.c:95-100` (DDR-981 §9) describes for the
SYSCALL swapgs windows: *"CS reads as ring 0 while the USER's GS base is still
active … this_cpu() would then read %gs:0 against a user GS base of 0, i.e.
linear address 0 in ring 0."* The predicted symptom and the observed one match
bit for bit.

**What that does NOT settle.** DDR-981 §9 reasons that `isr_common`'s CS-based
swapgs decision is *consistently* wrong in those windows — it declines to swap on
entry and declines again on exit — so an NMI landing there should be balanced and
leave GS as it found it. And the ordering in this capture argues against the NMI
prober being the trigger at all: `[percpu] gs FAIL` appears at log line 202,
while the first `[apfreeze]` NMI is at line 409. **The corruption precedes any
NMI in this boot.**

So the window is identified and the *state* is identified; the event that leaves
the kernel in it is not. Do not "fix the NMI race" on this evidence — DDR-981
already reasoned it balanced, and the artefact does not contradict that.

## 4. CORRECTION — the detector gap I first claimed here was NOT real

**This section originally said** that `GLOBAL_FORBIDDEN` carried only
`'percpu FAIL'`, that it could not match `[percpu] gs FAIL (syscall ctx)`, and
that therefore *"only `smoke-swapgs` would notice"* — so the primary event went
unseen and the gate failed three symptoms downstream.

**That is wrong, and it was wrong when written.** The list already carried a bare
`'gs FAIL'` entry, at position 54, well before this DDR:

```
$ git show d7d2794^:tools/qemu_runner/boot_test.sh | awk '/^GLOBAL_FORBIDDEN=/{f=1} f{print} f && /\)"$/{exit}' \
    | grep -o "'[^']*'" | tr -d "'" | grep -niE "gs |percpu"
54:gs FAIL
60:percpu FAIL
```

`[percpu] gs FAIL (syscall ctx)` contains the substring `gs FAIL`, so **every**
gate running through `boot_test.sh` already caught it. I grepped the list for
`percpu`, found only `percpu FAIL`, and concluded coverage was missing without
checking whether a *differently worded* entry covered the same string. The
failing `smoke-blk-integrity` boot did not hide its primary event; the global
list would have failed it on `gs FAIL` alone.

Found by the scanner in §8 printing **both** `gs FAIL` and `[percpu] gs FAIL` as
hits on the same line — i.e. the tool built to close the gap demonstrated the gap
was not there.

### 4.1 What of the two additions survives

| entry | verdict |
|---|---|
| `'[percpu] gs FAIL'` | **redundant** with the pre-existing `'gs FAIL'`. Kept, because §NON-NEGOTIABLE 6 makes the list append-only, but it buys nothing and is recorded here as such rather than left to look load-bearing. |
| `'[percpu] current FAIL'` | **genuine new coverage.** No pre-existing entry matches it: `'percpu FAIL'` does not (there is a word between), and there is no bare `'current FAIL'`. |

So one real hole was closed, not two, and the narrative that OPEN-2 hid behind a
missing sentinel does not hold. What DID hide it is in §8 — and that one is
measured, not inferred.

## 5. Is this a regression from DDR-1008/1009? No mechanism, and the site predates them

The failure appeared on the commit carrying DDR-1008 (compositor) and DDR-1009
(panic-path console lock + sentinels), so it had to be checked rather than
assumed.

| arm | kernel | result |
|---|---|---|
| current | `29c792a8b8f3b056` | 1 failure in 4 |
| pre-change | `bb9c6187a30bb0dd` | 0 failures in 6 |

**Those counts settle nothing** — Fisher's exact on 1/4 vs 0/6 gives p ≈ 0.40.
Stating them as evidence of no regression would be the arithmetic error §1.1 just
caught. What does carry weight is mechanism and provenance:

- **The RIP is identical to DDR-1006's CI capture**, which was taken on
  `bb9c6187a30bb0dd` — *before* either change existed. The wedge site is
  demonstrably pre-existing.
- **The failing path is untouched by the diff.** DDR-1008 is ring-3 compositor
  code, and in this gate its added work is one `printf` at boot plus two branch
  tests per frame (`draw_dock` returns immediately while `g_min_mask == 0`, which
  it always is here — nothing minimizes). DDR-1009 changes
  `console_panic_force_release`, reachable only *after* a panic, and this boot's
  corruption precedes its panic. Neither goes near `swapgs`, percpu, or `sys_mmap`.
- **It failed on its own pre-existing sentinel**, `blk integrity FAIL
  reference-read` — not on the `NEXUS KERNEL PANIC` string DDR-1009 added. The
  new sentinel did not manufacture this red.

Attributing it to the diff would need a mechanism nobody can name; attributing it
to OPEN-2 has an exact signature match on an older kernel.

## 6. What is NOT established, and one measurement that was lost

- **The source defect is not named.** "The SWAPGS discipline is unbalanced on
  some CPU at some ring-3 syscall entry" locates the fault; it does not say which
  instruction sequence loses it. §NON-NEGOTIABLE 3 forbids a fix on this.
  Candidate to examine first, because the backtrace passes straight through it:
  `isr_common.gs_kernel_in` — the ISR's own decide-whether-to-swapgs point — and
  an interrupt or NMI landing inside the syscall entry's swapgs window
  (`syscall_entry.asm`).
- **The two frozen CPUs are not both explained.** `ticks[11330,186,184,11279]`
  shows CPU 1 *and* CPU 2 stopped, at 186 and 184. Only one `[apfreeze]` CPU
  (cpu=1) is in the capture. Whether CPU 2 froze for the same reason is unknown.
- **17 of 20 campaign serial captures were lost — and the mechanism is now
  known.** `campaign_chunk.sh` snapshotted with

  ```sh
  if [ -n "$serial" ] && [ -f "$ROOT/$serial" ]; then
  ```

  It prepends `$ROOT/` **unconditionally**, so an *absolute* serial path becomes
  `/home/user/Prady4OS//home/user/Prady4OS/build/...`, which never exists — and
  the copy silently did nothing. The first chunk was invoked with a **relative**
  path and kept its captures; the resumed chunk used an absolute one and kept
  none. That is the whole of it: not a resume bug, a path-handling bug that only
  a relative path hides.

  Fixed here — both forms accepted, and a run whose capture path resolves to
  nothing now prints a WARNING naming the path instead of dropping it quietly. A
  silent no-op there is indistinguishable from a clean run with nothing to
  record, which is the same "reads as covered" failure as §4 and §8.

  The `[apfreeze]` conclusion in §1 survives regardless, because it rests on the
  ledger's PASS lines rather than on the captures.
- **The rate is not measured.** One failure in four on one kernel is an
  observation, not a rate.

## 7. The next instrument, named

Not another `smoke-smppreempt` campaign. The gate that reproduces is
`smoke-blk-integrity`, and the question is now narrow enough to instrument
directly:

1. **Make the SWAPGS probe continuous, not one-shot.** `gs_checked` fires once,
   on the first `sys_getpid` in the whole boot. It caught this only because the
   corruption happened to be early. A cheap `pc->self == pc` check on every Nth
   syscall entry would bound *when* GS goes bad, which one-shot cannot.
2. **Record which CPU.** The probe prints no cpu index; `[apfreeze]` says cpu=1,
   but nothing ties the two.
3. Then campaign `smoke-blk-integrity` at **N ≥ 36** (§1.1's power figure), with
   `SERIAL_LOG` pinned and `KEEP_SERIAL=1` **verified on the resume**, not just
   the first chunk.

---

## 8. The detector gap that IS real: `smoke-shell` applies no global sentinels

§4 retracted an inferred gap. This one is measured.

`smoke-shell` drives QEMU itself through a FIFO — it feeds PRISM a command
stream — and **never calls `boot_test.sh`**:

```
$ awk '/^smoke-shell:/{f=1} f && /boot_test\.sh/{c++} f && /^$/{f=0} END{print c+0}' Makefile
0
```

`GLOBAL_FORBIDDEN` lives inside `boot_test.sh` and is applied by it. So
`smoke-shell` applied **none of the 73 sentinels**: not `[apfreeze]`, not
`*** NEXUS KERNEL PANIC ***`, not `gs FAIL`.

That matters more than it sounds, because `smoke-shell` is **hygiene gate 8** —
CLAUDE.md requires `smoke-shell` 5/5 locally before *every* push. The gate a
session is told to trust last before pushing was blind to every global detector.

**Measured, not argued.** The §7 probe was built with a deliberate non-vacuity
mutant (`if (0)` in place of the health check) so that a healthy kernel prints
the failure line. That kernel — `6c81563d46114d5c` — printed:

```
[percpu] gs FAIL (syscall ctx) apic=0 num=6 gs0=0xFFFFFFFF80136D60 want=0xFFFFFFFF80136D60
```

and **`smoke-shell` reported PASS.** A kernel announcing a broken SWAPGS
discipline on every syscall passed the last gate before a push.

### 8.1 The fix, and why it is fail-loud

`tools/qemu_runner/scan_forbidden.sh <log> [label]` extracts `GLOBAL_FORBIDDEN`
from `boot_test.sh` and greps a capture with it; `smoke-shell` now runs it over
`build/shell_serial.log` before declaring PASS.

The extraction **refuses to report clean if it recovers fewer than 60 patterns.**
That is not defensive padding — it is the §NON-NEGOTIABLE 6 failure mode applied
to the reader instead of the writer. That rule exists because the list was
silently EMPTY for four commits and nothing noticed, since an empty list fails
nothing. A scanner that extracts zero patterns and prints "clean" would
reintroduce exactly that, one level up. It also uses an `awk` range ending at the
closing `)"` rather than a `sed` range keyed on the final entry — the keyed form
is what broke twice today when entries were appended.

Verified **three** ways: FAIL (naming each pattern) against the mutant capture;
clean, 73 patterns, against the healthy one; and — the arm that matters — a
deliberately broken extractor exits non-zero with *"Refusing to report a clean
scan against nothing"* rather than printing clean.

One bug found by that count. The first version reported **74** patterns against a
list of 73: `grep -o "'[^']*'"` was also capturing `printf`'s own format string,
`'%s\n'`, off the assignment line. Harmless in practice — no serial log contains
a literal `%s\n` — but a scanner whose pattern count does not match the list it
claims to apply cannot be audited, which is the entire point of the fail-loud
threshold. The `awk` guard now skips the assignment line.

### 8.2 What is still not covered

`smoke-selftest` also does not call `boot_test.sh`, but that is correct — it is
the meta-test *of* `boot_test.sh` and invokes it under controlled conditions.
Not a gap.

The other bespoke recipes were not audited. `smoke-shell` was chosen because it
is a mandatory pre-push gate; a full sweep of which gates do and do not apply the
global list is unbuilt work, and the count above (`0`) is the query that would
do it.


---

## 9. MEASURED — the §7 campaign, 36/36 clean, and what that is worth

Kernel **`9623c163cd479043`**, one hash recorded on **both sides of every run**
(the ledger's dual-hash column, which caught two mid-campaign rebuilds earlier
today).

```
36 runs, 36 PASS, 36/36 serial snapshots kept
gs FAIL captures:  0
apfreeze captures: 0
```

`smoke-blk-integrity` runs through `boot_test.sh`, so `[apfreeze]`,
`[percpu] gs FAIL` and `[percpu] current FAIL` are all `GLOBAL_FORBIDDEN`
entries — PASS implies their absence without needing the captures. The captures
were nevertheless kept (36/36, against 3/20 before the path fix) so the boots can
be read for anything else.

### 9.1 The detector was live, and that is shown two ways

A silent detector and a working one look identical in a clean run, so neither is
assumed:

1. **The one-shot probe printed on all 36.** Every capture contains
   `[percpu] gs OK (syscall ctx)`, so `sys_getpid` was reached and the GS check
   executed and passed on each boot.
2. **The continuous probe is proven by mutation, not by silence.** It prints only
   on failure by design, so its quiet here proves nothing on its own. What proves
   it works is §8's `if (0)` mutant (kernel `6c81563d46114d5c`), which made a
   healthy kernel emit
   `[percpu] gs FAIL (syscall ctx) apic=0 num=6 gs0=… want=…` with `gs0 == want`.

### 9.2 What 36/36 establishes, and what it does not

**Establishes:** the local per-run rate on this kernel is below ~8% at 95%
confidence — `0.92³⁶ ≈ 0.049`, which is the power figure §1.1 derived *before*
the campaign was run, from CI's observed ≈0.08.

**Does NOT establish that the defect is gone**, and three things say so:

- **It reproduced on the FIRST run** of an earlier session on kernel
  `29c792a8b8f3b056` (§5), then 3 more passed. One failure in four is a single
  event; it cannot separate p=0.25 from p=0.05, so "the rate fell" is not a
  comparison these sample sizes support.
- **The mechanism is untouched.** Nothing in DDR-1010, DDR-1012 or DDR-1013
  changed `swapgs`, percpu, or the syscall entry path's discipline. A defect
  whose cause is unaddressed does not stop existing because a sample came back
  clean.
- **The instrument may perturb what it measures.** §7's probe adds a
  `this_cpu()` read and a compare to the top of **every syscall**. The failure is
  a timing-sensitive race in the syscall entry path, and that is precisely where
  work was added. 0/36 on a kernel carrying the probe is therefore weaker
  evidence about the *un*probed kernel than the arithmetic alone suggests.

### 9.3 The next experiment, if one is run

Not another 36 on this kernel. The question §9.2 raises is answerable: campaign
`smoke-blk-integrity` on **`29c792a8b8f3b056`** — the pre-probe kernel, and the
one on which the failure was actually observed. If it fails there at a rate the
probe kernel does not show, the probe perturbs the race and the instrument needs
rethinking. If it too comes back clean, the 1-in-4 was a small-sample artefact
and the local rate was always low.

**Either answer is worth having and neither is assumed here.** What ships today
is: a located mechanism, an armed continuous detector, and a bounded local rate.
The source defect is still **not named**.
