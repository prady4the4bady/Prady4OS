= DDR-943 — the eager 8 MiB user stack exhausts a 128 MiB machine at ~15 processes

> **STATUS DOWNGRADED — see "The evidence does not yet support this" at the
> bottom. The mechanism is real and the arithmetic holds, but the causal claim
> is NOT established, and two statements in this document are wrong.**

**Status:** ~~ACCEPTED~~ **PLAUSIBLE, UNCONFIRMED** (mechanism verified in
source; causation not measured).
**Date:** 2026-08-16
**Evidence:** CI run 31952413936 (tip `62c65cd`), shard 0 — 11 x
`[boot-load] FAILED <name> reason=elf rc=8`.
**Lineage:** DDR-941 (named the failures) → DDR-942 (recorded them) →
**DDR-943 (this: root cause)**. Distinct from the scheduler defect per §6.0-C.

## The observation

11 probes failed to load, all `rc=8` = `ELF_E_NOMEM` ("out of frames /
address space", `elf.h:40`), including `INIT.ELF` and `PRISM.ELF`. The
immediately preceding green run had **zero**.

## The root cause

`kernel/exec/elf.c:189-200` maps the **entire 8 MiB user stack eagerly**, one
frame per page, at load time:

```c
/* --- user stack: 8 MiB RW+NX, guard page below, SysV ABI initial frame --- */
for (uint64_t va = USER_STACK_BOT; va < USER_STACK_TOP; va += PAGE_SIZE) {
    void *frame = ptnode_alloc();
    if (!frame) { vmm_destroy_address_space(as); return ELF_E_NOMEM; }
    …
}
```

`USER_STACK_SIZE = 8 MiB` (ADR-021, `elf.c:73`), so this is
**2,048 frames per process**, whether or not the process ever touches them.
`ptnode_alloc()` returning NULL is the *only* source of `ELF_E_NOMEM` on this
path — the observed `rc=8` **is** this loop failing.

## The arithmetic

**No gate passes `-m` to QEMU.** All 22 `qemu-system-x86_64` invocations in the
Makefile use the default, which for x86_64 is **128 MiB**:

```
grep -c "qemu-system-x86_64" Makefile  -> 22
grep -c -- "-m [0-9]"        Makefile  ->  0
```

**The RAM figure is ASSUMED, not yet measured — read this before relying on the
table.** No gate passes `-m`, so the machine takes QEMU's documented x86_64
default of 128 MiB. That default is *documented behaviour, not an observation
from this tree*: the kernel prints no total-RAM line at boot, so it could not be
confirmed from any existing log. The `pmmtot=` field added with this DDR reports
`pmm_total_page_count()` directly and settles it on the next run. If `pmmtot`
comes back materially different from ~32,768, **the arithmetic below is wrong
and this DDR must be re-derived** — the mechanism (2,048 eager frames per
process) would survive, but the predicted failure count would not.

### MEASURED (supersedes the assumption above)

`smoke-wmorder`, passing boot, tip `e296030` + this instrument:

```
[hb] t=10500 … rqdepth=6 rqcpus=1 rqq=1 rqpres=1 pmmfree=14316 pmmtot=28630
```

**`pmmtot=28630`, not the assumed 32,768** — 128 MiB nominal less firmware,
kernel image and reserved regions, so the 128 MiB premise holds in substance
while the derived figure did not. The corrected arithmetic is **worse**, not
better:

| quantity | value |
|---|---|
| total managed frames | **28,630 (measured)** |
| free at steady state after boot | **14,316 (measured)** — half of RAM already gone |
| frames per user process (stack alone) | **2,048** |
| processes that fit in *all* RAM | **~14** |
| processes that fit in the free half | **~6.9** |

So on a healthy, *passing* boot there is headroom for **seven more processes**,
against a probe list of ~30. The defect is not marginal.

| original estimate | value |
|---|---|
| frames per user process (stack alone) | **2,048** |
| processes before RAM is gone (stacks only) | **16** |
| minus kernel image, page tables, heap, DMA rings | **~15 or fewer** |
| probes booted via `user_boot_from_sfs` | **~30** |
| predicted failures | **~11-15** |
| **observed failures** | **11** |

The prediction and the observation agree without fitting. This is not a
contributing factor; it is sufficient on its own.

## Why it is intermittent (and why the green run showed zero)

Frames are returned when a process exits (`vmm_destroy_address_space`). Whether
probe N finds 2,048 free frames depends on how many earlier probes have already
**exited** by the time it loads. That is a race between probe lifetime and boot
sequencing, so the same image can boot clean or lose the tail of its probe list
depending on timing — exactly the intermittency observed, and exactly why a
green run proves nothing here.

It also explains why the failures arrive as a **contiguous tail** (the last 11
of ~30) rather than scattered.

## DDR-829/831 were NOT fixed

The handoff records DDR-829/831 as having identified this eager-stack problem.
The loop is still there, unchanged, mapping all 2,048 frames. **Verified by
reading `elf.c:189-200`, not by trusting the record.** Any plan that assumed
this was fixed is working from a false premise.

## What would refute this

- Free-frame count at the moment of a failure being **>> 2,048** (then
  `ptnode_alloc` is failing for some reason other than exhaustion).
- Failures occurring with **few** processes resident.
- `rc=8` arriving from a path other than this loop.

## Instrument (this slice — diagnosis only, no fix)

Add `pmmfree=` / `pmmtot=` to the `[hb]` line from the existing
`pmm_free_page_count()` / `pmm_total_page_count()` accessors
(`pmm.c:245-246`). This converts the refutation criteria above into a direct
reading and shows the free-frame count collapsing across boot.

No new sentinel (fields on an existing `[hb]` line), so the count stays at 160.

## The fix, deliberately NOT in this slice

Lazy stack mapping: map one page at `USER_STACK_TOP - PAGE_SIZE` for the SysV
initial frame and fault the rest in on demand. That is a real change to the
page-fault handler and the ELF loader, it needs its own ADR (it touches
ADR-021's stack contract), and it must not ride along with a diagnostic commit.

**A `-m 512` on the gates is NOT the fix** — it would hide a real 2,048-frames-
per-process defect behind a bigger machine and make the OS unable to run ~15
processes on real 128 MiB hardware. It may be worth doing *separately* so CI
stops failing for a reason unrelated to what each gate tests, but it must be
recorded as masking, not fixing.

## Relationship to the scheduler defect (§6.0-C)

**Not merged.** The blk workers' `spawned=4/4` shows `kmalloc` (heap) was fine
while the PMM (frames) was failing — different allocators. And `done=0x0` shows
those workers never reached even their failure path, which frame exhaustion
does not explain. Two defects, same boot. A common cause may yet exist; it has
not been measured, so it is not claimed.

---

## The evidence does not yet support this (added after CI run 31958185299)

Two corrections to this document, both mine.

### Correction 1 — `ptnode_alloc` is NOT the only source of `ELF_E_NOMEM`

This DDR states that `ptnode_alloc()` returning NULL "is the *only* source of
`ELF_E_NOMEM` on this path, so the observed `rc=8` **is** this loop". That is
false. The same loop has a second one:

```c
if (vmm_map_in(as, va, phys, VMM_USER|VMM_RW|VMM_NX) != 0) {
    ptnode_free(frame); vmm_destroy_address_space(as); return ELF_E_NOMEM;
}
```

`vmm_map_in` failing returns `ELF_E_NOMEM` too, and earlier segment-mapping
code can also produce it. `rc=8` therefore does **not** identify the stack loop
on its own.

### Correction 2 — the `pmmfree` reading neither confirms nor refutes this

CI run 31958185299 has `INIT.ELF` failing at `t=2746` with the next heartbeat at
`t=3000` reporting `pmmfree=19645` — nearly 20,000 free frames, far above the
2,048 a stack needs. My first reading of that was that it **refutes** this DDR.
**That reading was wrong**, for two reasons:

1. Every `ELF_E_NOMEM` path calls `vmm_destroy_address_space(as)` on the way
   out, **returning up to 2,047 frames**. The count recovers by construction.
2. The heartbeat is **254 ticks after** the failure, and `[hb]` samples only
   every 500 ticks.

So `pmmfree=19645` is the *post-cleanup* figure. A transient exhaustion that
heals via its own error path is **invisible** at this sampling rate. The
instrument was too coarse to test the claim it was added to test — the same
class of error as DDR-942's first-draft `rqdepth` criterion.

### What is still true

- The eager loop is real: 2,048 frames per process, verified at `elf.c:189-200`.
- `pmmtot=28630`, `pmmfree=14316` at steady state — measured. ~14 processes fit
  in all RAM, ~7 in the free half, against ~30 probes.
- DDR-829/831 did not fix it.

### What is now needed

`pmmfree=` is printed **on the `[boot-load] FAILED` line itself** (DDR-945), so
the free count is read at the instant of failure rather than up to 500 ticks
later. Then:

- `pmmfree` at failure **< 2,048** ⇒ this DDR is confirmed.
- `pmmfree` at failure **>> 2,048** ⇒ genuinely refuted; the failure is
  `vmm_map_in` or an address-space limit, not frame exhaustion, and the hunt
  moves to the VMM.

Until that reads out, this DDR is a **plausible mechanism with correct
arithmetic and no proof of causation**, and must not be cited as the root cause
of the `rc=8` failures.
