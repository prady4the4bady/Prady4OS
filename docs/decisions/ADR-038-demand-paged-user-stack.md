= ADR-038 — demand-paged user stack (supersedes ADR-021's eager-stack clause)

> **STATUS: DESIGN INCOMPLETE — read "IMPLEMENTED, MEASURED, THEN REGRESSED"
> at the bottom FIRST. The frame win is real (~12,200 frames recovered) but the
> implementation regressed three gates, and the cause is a flaw in this ADR's
> own analysis: it never considered the paths that deliberately never fault
> (ADR-022 copyin/copyout). Code is stashed, NOT committed.**

**Status:** ~~ACCEPTED~~ **PROPOSED / DESIGN INCOMPLETE.** Would supersede the
stack-mapping clause of **ADR-021** only; ADR-021's W^X segment rules are
unchanged and remain binding. Not in force — the design needs option 1, 2 or 3
from the bottom section before it can supersede anything.
**Date:** 2026-08-16
**Driver:** DDR-943, **CONFIRMED** by measurement (CI 31989445727):
`pmmfree=2096` against `pmmtot=28630`, with a per-process cost of exactly
2,048 frames. Written before any code, per R7/R18.
**Numbering:** ADR-038 verified free (highest existing is ADR-037).

## Problem

`kernel/exec/elf.c:189-200` maps the **entire 8 MiB user stack eagerly**, one
frame per page, at load time:

```c
for (uint64_t va = USER_STACK_BOT; va < USER_STACK_TOP; va += PAGE_SIZE) {
    void *frame = ptnode_alloc();
    if (!frame) { vmm_destroy_address_space(as); return ELF_E_NOMEM; }
    if (vmm_map_in(as, va, phys, VMM_USER|VMM_RW|VMM_NX) != 0) { …; return ELF_E_NOMEM; }
}
```

**2,048 frames per process, touched or not.** Measured consequences:
`pmmtot=28630`, steady-state `pmmfree≈14300`, so ~7 further processes fit and
~14 fit in all of RAM — against ~30 ring-3 probes. The observed floor reached
`pmmfree=2096`, i.e. **one stack's headroom**.

## Decision

Map **two** pages at load time and fault the rest in:

| region | at load | rationale |
|---|---|---|
| `USER_STACK_TOP - PAGE_SIZE` (top page) | **mapped** | `elf.c:206-207` writes argc/argv/envp/auxv into it via its identity view; the SysV initial frame must exist before first entry |
| `[USER_STACK_BOT, USER_STACK_TOP - PAGE_SIZE)` | **unmapped** | faulted in on first touch |
| `[USER_STACK_BOT - PAGE_SIZE, USER_STACK_BOT)` guard | **unmapped, never fillable** | overflow must fault, not silently extend |

**Per-process cost at spawn: 2,048 frames → 1 stack frame** (plus the page-table
nodes the mapping itself needs, which are unchanged in kind and far fewer).

## The 11 checklist answers (§H)

1. **New per-process frame cost at spawn:** 1 stack frame + PT nodes, versus
   2,048 + PT nodes. ~2,047 frames returned per process.
2. **How #PF distinguishes a stack-growth fault from a real fault:** four
   conditions, ALL required — vector 14; `(cs & 3) == 3` (ring 3);
   `(err & 1) == 0` (**not-present**, distinguishing it from ADR-021's
   present-but-protection faults and from the existing COW path which requires
   `err & 1 == 1`); and `CR2` in `[USER_STACK_BOT, USER_STACK_TOP)`. Anything
   outside that range, or any present-page violation, falls through unchanged
   to the existing kill path.
3. **Guard page address:** `[USER_STACK_BOT - PAGE_SIZE, USER_STACK_BOT)` =
   `[0x8FFFFFF000 - 0x1000, 0x8FFFFFF000)` given `USER_STACK_TOP 0x9000000000`
   and `USER_STACK_SIZE 8 MiB`. It is **below** `USER_STACK_BOT`, therefore
   **outside** the range test in (2), so a guard-page touch can never be
   satisfied by the growth path. It falls to the kill path. This is the
   property that makes the guard real rather than decorative.
4. **musl `__init_tls` interaction:** static musl places its TLS block in a
   `builtin_tls` static buffer, not on the stack, so demand paging does not
   change TLS setup. What it *does* touch is the initial frame — which stays
   eagerly mapped precisely for that reason (row 1 of the table).
5. **`__init_ssp` interaction:** reads `AT_RANDOM` from the auxv, which lives
   in the eagerly-mapped top page. Unaffected. (The auxv is separately
   incomplete — see "Known adjacent defect" below. Not fixed here.)
6. **OOM inside the fault handler:** `pmm_alloc_page()` can fail during growth.
   The handler must **kill the faulting thread** with a named diagnostic, never
   panic and never silently resume — a silent resume would re-fault forever.
   S2 ("bounded everything … never a panic") is binding here.
7. **W^X:** growth pages are mapped `VMM_USER | VMM_RW | VMM_NX`, identical to
   the eager mapping. ADR-021's W^X invariant is preserved unchanged.
8. **Zeroing:** every growth page must be zeroed before mapping. An unzeroed
   page would leak a previous process's memory into a new stack — a security
   defect, not a performance detail.
9. **Teardown:** `vmm_destroy_address_space` walks the page tables and frees
   what is mapped, so a partially-populated stack frees correctly with no
   change. Fewer mapped pages means strictly less teardown work.
10. **SMP:** two threads of one process can fault on the same stack page
    concurrently. The map step must tolerate a lost race — if the page became
    present while we were allocating, free the spare and resume rather than
    double-mapping.
11. **Rollback:** revert is a single-hunk restore of the eager loop. Per R11 the
    revert is not verified until the gate is re-run after it.

## Gate — `smoke-stack-demand`, three arms, distinct kernel SHAs (R6)

- **Arm A (baseline, feature absent):** eager map — spawning past the frame
  ceiling produces `ELF_E_NOMEM`. **Must FAIL** the new assertion.
- **Arm B (deliberate defect):** demand map with the guard page *mapped*
  (guard fillable). Stack overflow silently extends instead of faulting.
  **Must FAIL.** This arm is what proves the gate tests the guard and not just
  "it booted".
- **Arm C (correct):** demand map + unfillable guard. Full stack use succeeds,
  overflow kills only the faulting thread, and `pmmfree` stays far above 2,048.
  **Must PASS.**

Assertion is on `pmmfree` **and** on the guard behaviour; a gate that only
checked "boots OK" would pass all three arms and prove nothing.

Per R10 this touches the #PF path and shared allocator state ⇒ **20/20 local**
before shard registration, then CI.

## Known adjacent defect — NOT fixed here (§6.0-C / R13)

`elf_load` builds an auxv of only `AT_PAGESZ` + `AT_NULL` (`elf.c:218-221`),
omitting `AT_PHDR`/`AT_PHNUM`/`AT_RANDOM`. That is a **separate** defect with a
separate root cause and gets its own DDR (directive ITEM 4). It is named here
only because both live in the same function and a future reader will otherwise
assume this ADR covered it. **It does not.**

## What would refute this design

- `pmmfree` not improving materially after the change ⇒ the eager stack was not
  the dominant frame consumer and DDR-943's arithmetic is wrong somewhere.
- Growth faults appearing for addresses outside the stack range ⇒ the range
  test is wrong or the stack VA constants are not what this ADR assumes.
- Any measurable increase in `[trap] user #PF` kills on previously-passing
  gates ⇒ the range test is too narrow and is rejecting legitimate growth.

---

## IMPLEMENTED, MEASURED, THEN REGRESSED — code NOT shipped

Implemented exactly as designed above and measured. Two results.

### The frame win is real and large

`smoke-agent-click` (PASS) with the demand-paged stack:

```
before:  pmmfree ~14316 steady, floor 2096   (of pmmtot=28630)
after:   pmmfree  26538 .. 27063             (of pmmtot=28630)
```

**~12,200 frames recovered** — consistent with ~6 resident processes x 2,048.
Free RAM went from ~50% to ~93%. `[trap] user #PF` kill count was **unchanged
at 2** (same in the pre-change serial log), so the range test was not rejecting
legitimate growth.

### But it REGRESSED three gates

`smoke-blkmq`, `smoke-rqstress-liveness`, `smoke-blk-integrity` all **FAILED**.
These are on the known-intermittent list, so per the obstacle rules I ran the
discriminating test rather than assuming — stash the change, rebuild, re-run:

```
PRE-CHANGE blkmq:    PASS
PRE-CHANGE rqstress: PASS
```

**Both pass without the change. The regression is mine.** Caught by the
revert test before push, not after.

### The design flaw the checklist missed

`vmm_user_range_ok` (ADR-022) validates every user pointer that crosses
copyin/copyout **without faulting** — its contract is explicitly "never
allocates and never faults", returning 1 only if every page in the range is
already *present*.

With a demand-paged stack, a stack page that has not yet been touched is **not
present**. So any syscall handed a pointer into an untouched stack page now
fails validation and returns `-EFAULT`, where before the eager map guaranteed
presence. Kernel threads calling into probes with stack-resident buffers are
exactly the shape of `blkmq`/`blkint`/`rqstress`.

**This is a real architectural interaction and item 2 of my own 11-item
checklist should have caught it.** The checklist asked how the #PF handler
distinguishes a growth fault — it never asked which paths deliberately *avoid*
faulting. Recording that gap as the lesson, not just the bug.

### Status

**ADR-038 design is INCOMPLETE. Code is stashed, not committed.** The frame win
justifies finishing it, but it needs one of:

1. **Pre-fault on validation** — have `vmm_user_range_ok` (or its callers)
   populate stack-range pages before validating. Breaks ADR-022's never-faults
   contract, so it needs an ADR-022 amendment, not a quiet edit.
2. **Fault-tolerant copyin/copyout** — resolve a not-present stack page inside
   the copy path. Larger change; must not weaken the fail-closed property that
   makes `vmm_user_range_ok` a security boundary.
3. **Eagerly map a small stack window** (e.g. top 16-32 pages) and demand-page
   only beyond it. Keeps ADR-022 intact and still recovers ~2,000 frames per
   process. **Cheapest and least invasive — evaluate this first.**

Option 3 is not a compromise on correctness: it bounds the syscall-visible
stack region to what a plausible syscall buffer occupies while still deleting
the overwhelming majority of the eager cost. Its risk is a buffer deeper than
the window, which is measurable rather than speculative.

**Next step is a measurement, not a choice:** instrument `vmm_user_range_ok`
failures by address so the actual stack depth syscall buffers reach is known.
That number sizes option 3 and either confirms or refutes the mechanism above.
