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

## 4. The detector gap that hid it for this long

`GLOBAL_FORBIDDEN` already carried `'percpu FAIL'`. It does **not** match either
line above: the printed strings are `[percpu] gs FAIL (syscall ctx)` and
`[percpu] current FAIL (syscall ctx)` — there is a word in between.
`'percpu FAIL'` matches only the SMP-boot form `[smp] cpu N percpu FAIL`.

So the only gate that would notice is `smoke-swapgs`, which names `gs FAIL` in
its own `FORBIDDEN_SENTINEL` (`Makefile:2886`). In every other gate the kernel
could announce a broken SWAPGS invariant and be believed.

That is what happened here: the boot said `gs FAIL`, then `#GP`'d, then froze two
CPUs, and the gate finally failed **three symptoms downstream** on
`blk integrity FAIL reference-read`. Every previous OPEN-2 investigation started
from that third symptom.

**An entry that looks like it covers a family and covers one member is worse than
no entry, because it reads as covered.** `'[percpu] gs FAIL'` and
`'[percpu] current FAIL'` are now their own entries (71 → 73). Verified safe: no
gate expects either string, and `smoke-swapgs` already treats them as failures.

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
- **17 of 20 campaign serial captures were lost.** The resumed chunk did not
  snapshot them, so only runs 1–3 have `.serial.log` files. The `[apfreeze]`
  conclusion survives (§1), but nothing else about those boots can be read. This
  is the DDR-1000 §10 lesson recurring in a new form: the snapshot was configured
  for the first chunk and not carried into the resume.
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
