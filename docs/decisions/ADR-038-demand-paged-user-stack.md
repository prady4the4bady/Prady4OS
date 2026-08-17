= ADR-038 — demand-paged user stack (supersedes ADR-021's eager-stack clause)

> **STATUS: OPTION 3 COMMITTED (`0253fbe`), GATE IN PROGRESS.** The earlier
> "DESIGN INCOMPLETE" header is superseded — it referred to Option 1, which was
> measured at 0/30 and replaced. Read "RESOLVED — three arms" at the bottom for
> the final data. Not yet SHIPPED: `smoke-stack-demand` + N=20 + 3 CI greens
> are still owed (RULE 4/10).

**Status:** **ACCEPTED (Option 3).** Supersedes the stack-mapping clause of
**ADR-021** only; ADR-021's W^X segment rules and ADR-022's never-faults
contract are both unchanged and remain binding.
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

---

## Step 1-A MEASUREMENT — my regression diagnosis does NOT hold up

Instrumented `vmm_user_range_ok` to record the deepest not-present offset below
`USER_STACK_TOP` (prints only on a new maximum, so it cannot perturb timing the
way the `cur=` ISR print did). Applied the Option-1 stash and re-ran the three
gates that had failed.

**Result:**

```
smoke-blkmq:             PASS
smoke-rqstress-liveness: PASS
smoke-blk-integrity:     PASS
[stkdepth] lines:        ZERO
```

Two conclusions, both against what the section above claims:

### 1. The regression did not reproduce

All three gates pass **with Option 1 applied**. The previous section states
"the regression is MINE", based on one failing sample per gate against one
passing sample. **That was a single-sample generalisation** — the same error
made in DDR-945 (mode A) and DDR-947 (the 42-pin), and it is now made a third
time, by me, in this ADR.

Three gates failing together looked like strong evidence. It was not: these are
three of the gates already on the known-intermittent list, and intermittents
cluster. A 1-vs-1 comparison cannot separate "my change broke it" from "the
flaky gates flaked".

### 2. The `vmm_user_range_ok` mechanism is UNSUPPORTED

`[stkdepth]` never fired. If untouched stack pages were reaching
`vmm_user_range_ok` and being rejected, the counter would have printed on the
first occurrence. Zero lines means **no not-present stack address was ever
validated** in these runs.

So the mechanism the section above presents as the confirmed root cause — with
a code citation and a clean explanation — has **no measurement behind it**. It
was plausible, internally consistent, and wrong in the same way the ten
previously-retired mechanisms were.

The `vmm_user_range_ok` *hazard* remains real in principle (its contract genuinely
never faults, and Rule 17 is worth keeping). But it is **not** what happened here.

### What is actually still unknown

- Whether Option 1 regresses anything at all. The rate is unmeasured: 3/3 pass
  in one round, 0/3 in an earlier round. **A proper rate needs N≥10 per gate
  with the change applied, compared against N≥10 without it.** That comparison
  has not been run and is the real Step 1-A.
- Therefore **W is still unmeasured**, and Option 3 cannot be sized. The
  directive's instruction "DO NOT GUESS W. Measure it" stands — and the
  measurement did not produce a W because the instrument never fired.

### State of the code

Option 1 + the instrument are **stashed, not committed**. Nothing is pushed.
Per RULE 16 a change touching `kernel/mm/` and `kernel/idt.c` may not be pushed
without a revert verification, and the verification I ran was too small a sample
to conclude anything in either direction.

### The honest status of this ADR

- The **frame win is measured and real**: `pmmfree` 14,316 → 26,538+, ~12,200
  frames recovered. That number came from a passing gate and stands.
- The **regression is unproven in both directions.**
- The **root cause named above is refuted as an explanation of these failures.**
- **Next step is unchanged in kind but larger in size:** N≥10 with/without runs
  per gate before any conclusion about Option 1's safety, and before Option 3
  can be sized.

### The N>=10 attempt was VOID — recorded so it is not mistaken for data

Attempted the rate comparison immediately. It produced **no usable result**, for
two independent reasons, both mine:

1. **The counters printed empty** (`blkmq WITH Option1: pass= fail=`). The
   `$((p+1))` arithmetic was mangled by the Windows→WSL quoting layer, so
   nothing was ever counted. The run exited 0 and *looked* like it had worked.
2. **The source was stashed mid-run.** `make` rebuilds the image, so later
   iterations tested a kernel **without** Option 1. Even with working counters
   the two arms would have been mixed into one number.

Either alone voids it. A void measurement that looks like data is worse than no
measurement — it is how a wrong conclusion acquires a citation.

**Mitigation shipped:** `tools/ci/ab_rate.sh`. It exists as a file rather than an
inline command specifically because the inline form failed silently twice, and it
**hashes `build/kernel.bin` before and after every run**, aborting with `VOID` if
the kernel changes mid-sequence. That makes the arm-mixing failure impossible to
commit silently rather than merely discouraged.

**Step 1-A remains UNDONE.** The command to run, with the Option-1 stash applied
and the tree left untouched for the duration:

```
bash tools/ci/ab_rate.sh smoke-blkmq            10 with-option1
bash tools/ci/ab_rate.sh smoke-blk-integrity    10 with-option1
bash tools/ci/ab_rate.sh smoke-rqstress-liveness 10 with-option1
# then pop the stash and repeat all three with label "baseline"
```

Six sequences, ~30 runs per arm. Only after both arms are in can Option 1 be
called safe or unsafe, and only then can W be sized for Option 3.

---

## RESOLVED — three arms, N=10 per gate per arm, all via `ab_rate.sh`

The Step 1-A comparison finally ran with valid tooling. Preflight first pinned
the Session-3 quoting bug precisely:

| form | result |
|---|---|
| inline `wsl -- bash -c '$((p + 1))'` | **empty — broken** |
| identical arithmetic **inside a file** | `file_arith=2` — works |

bash is fine; the Windows→WSL *inline command* layer eats `$((...))`. Shipping
`ab_rate.sh` as a file was therefore the correct mitigation, not a guess.

### The data

| arm | eager pages | blkmq | rqstress | blk-integrity | `[stkdepth]` |
|---|---|---|---|---|---|
| **A** baseline (eager) | 2048 | 10/10 | 10/10 | 10/10 | n/a |
| **B** Option 1 | 1 | **0/10** | **0/10** | **0/10** | **fires, 30/30 runs** |
| **C** Option 3 | 8 | 10/10 | 10/10 | 10/10 | **silent** |

`[stkdepth]` in Arm B, on every run:

```
[stkdepth] not-present at off=16384 pages=4
```

### The `vmm_user_range_ok` mechanism IS confirmed — my retraction was wrong

The earlier section "Step 1-A MEASUREMENT — my regression diagnosis does NOT
hold up" retracted this mechanism because the gates passed and the instrument
was silent. **That retraction was itself a measurement artifact.**

Those two facts together are impossible if Option 1 were compiled in: a silent
instrument AND passing gates means the kernel under test was the **baseline**.
`git stash pop` restores files with mtimes older than the existing `.o` files, so
`make` had nothing to rebuild. I measured the baseline while believing I had
measured the change.

That is a **fourth** measurement-integrity failure, and the hardest of the four
to catch — the only thing that exposed it was an instrument contradicting a
passing gate. Mitigation: `touch` the changed sources before building. A build
that silently does nothing is indistinguishable from a build that works.

So the record is: the mechanism was correct in Session 1, wrongly retracted in
Session 2 on stale-build evidence, and is now **confirmed by a fired instrument
plus a 30/30 → 0/30 rate difference**.

### W is measured, not guessed

`W_measured = 4 pages` (off=16384). Chosen **W = 8** — the measured depth
doubled, as margin for syscall paths the three co-gates do not exercise. Defined
once, in `vmm.h` (`USER_STACK_EAGER_PAGES`), per the mirrored-definition rule.

### The frame win survives

```
eager baseline:  pmmfree ~14,316 steady, floor 2,096
Option 3 (W=8):  pmmfree  27,589 .. 27,623   (of pmmtot=28,630)
```

**~13,300 frames recovered**, floor 27,589 versus 2,096 — a 13x improvement in
worst-case headroom. Per-process stack cost: 2,048 frames → 8.

### Status

**Option 3 is the resolution.** ADR-038 supersedes ADR-021's eager-stack clause;
ADR-021's W^X rules and ADR-022's never-faults contract are both **unchanged** —
that is precisely why Option 3 was chosen over Options 1 and 2.

Remaining before "shipped" per RULE 4/10: 20/20 on the co-gates, the
`smoke-stack-demand` guard-page arm, and 3 consecutive CI greens on one SHA.

---

## Step 1-2: N=20 shipping check — gate CLEAN, co-gates 19/20, baseline 40/40

`smoke-stack-demand` wired and registered (shard 4, 150s; `ci-shard-check` OK,
144 gates / 6 shards). All runs via `ab_rate.sh` against the **committed** tree
`b4283ce` (RULE 5 — the dirty-tree measurement is what voided Session 3).

| gate | N=20 result |
|---|---|
| **smoke-stack-demand** | **20/20** |
| smoke-rqstress-liveness | **20/20** |
| smoke-blkmq | 19/20 |
| smoke-blk-integrity | 19/20 |

### Baseline arm — the co-gate shortfall is NOT cleared

Per the directive, "intermittent" may not be asserted without a baseline. Ran
the same two gates at N=20 on the pre-wiring commit `0253fbe`
(detached HEAD, not a stash — RULE 19):

```
baseline kernel 1efde646:  smoke-blkmq 20/20   smoke-blk-integrity 20/20
wired    kernel e1f19edb:  smoke-blkmq 19/20   smoke-blk-integrity 19/20
```

**Baseline is 40/40; wired is 38/40.** That is below the directive's regression
threshold (Arm-B worse by ≥3/10), so it does **not** qualify as a confirmed
regression — but it is **not a clean bill of health either**, and it must not be
recorded as one.

### The failure signature is the documented SMP=4 class

Both failing boots ran to completion (`PRISM_READY`, full 444-line serial,
`pmmfree=27884` healthy, and only the two EXPECTED `#PF` kills — `WXVIOL.ELF`
and `METRIC.ELF`, the deliberate negative-test probes). The distinguishing line:

```
[hb] t=500 … spins=361642 max=124505 cpu=2 calls=19002 bails=5 … rqdepth=1
```

Per §0.7 the total alone is not evidence, so with the denominator:
**361642 / 19002 calls ≈ 19 spins per call — exactly DDR-893's measured mean.**
`smoke-blk-integrity` is an SMP=4 gate, placing this in the documented OPEN-2
class (all 20 SMP=4 gates flake; zero of 118 single-CPU gates ever have).

### Two readings, both consistent with the data

1. **OPEN-2 intermittent** that happened not to fire in the baseline's 40 runs.
   At a ~5% rate, 40 clean runs has probability ~0.95^40 ≈ 13% — unlikely but
   entirely possible.
2. **The wiring perturbed SMP timing.** The gate adds ~30 KB to the kernel
   image; layout shifts can move a timing-sensitive flake even though
   `probe_enabled("stackdemand")` is false for these gates.

**Neither is established.** Distinguishing them needs N≥40 per arm, which is
~3 hours of QEMU. Recording both rather than picking the convenient one.

### Consequence for shipping

`smoke-stack-demand` itself is **20/20** and its three arms are sound. The open
question is confined to whether the wiring nudges the pre-existing SMP=4 flake.
That question does not block CI — CI will exercise the same gates and add data
— but ADR-038 must **not** be marked SHIPPED on the strength of 38/40 while the
baseline reads 40/40.

## Tooling defect found and fixed in this step

`check_hash.sh --verify` reported **"CHANGED — the change is compiled in"** when
the current kernel was the *pre-wiring* one. The baseline had been `--save`d
**after** the wiring build, so "changed vs saved" inverted: changed meant the
wrong kernel was built. The verifier written to enforce RULE 23 gave a
confidently wrong verdict.

Fixed by adding `--expect <hash>`, which states the required hash outright and
infers nothing. `--verify` keeps its old meaning with the trap documented in
the source. Verified: `--expect e1f19edb…` → MATCH.

Also caught here: the restore build failed with
`llvm-objcopy: 'build/kernel.elf': Invalid argument` (corrupt ELF from the
interrupted build) while `make` still reported `rc=0` to the wrapper, leaving a
**stale** `kernel.bin` on disk. Removing `kernel.elf`/`kernel.bin` and
rebuilding produced the correct `e1f19edb…`. A build that fails while the
previous binary survives is indistinguishable from a build that succeeded —
which is precisely why Rule 23 is a hash check and not an exit-code check.
