# DDR-829 — the eagerly-mapped 8 MiB user stack caps the system at ~13 processes

**Status:** REJECTED — the arithmetic below is wrong; see "Refutation"
**Date:** 2026-08-04
**Governs:** `kernel/exec/elf.c` `USER_STACK_SIZE`
**Relates to:** ADR-021 (W^X, guard pages), ADR-022 (demand paging deferred), OPEN-11

## The defect

`elf_load` maps the **entire** user stack eagerly, one `ptnode_alloc()` per page:

```c
#define USER_STACK_SIZE  (8ull * 1024 * 1024)       /* 8 MiB */
for (uint64_t va = USER_STACK_BOT; va < USER_STACK_TOP; va += PAGE_SIZE) {
    void *frame = ptnode_alloc();
    ...
}
```

That is **2,048 frames per process**, allocated up front, whether or not the
process ever touches them.

The arithmetic that OPEN-11 comes down to:

```
PMM free frames   0x6F56 = 28,502 frames  (~111 MiB)
per process       2,048 frames            (8 MiB, eagerly)
ceiling           28,502 / 2,048 = ~13 processes
actually spawned  ~29 ring-3 threads
```

We ask for more than twice what the allocator can serve. The failure is
therefore **guaranteed by arithmetic**, and its *timing* is nondeterministic:
whichever process loses the allocation race is the one that dies. That is why
adding the `acctest` probe in `98fd2f8` broke **`smoke-sha256`** — a different
probe entirely — and why the same image passes on one run and fails on the next.

## Why not the other fixes

- **Give QEMU more RAM.** Rejected. It hides a real kernel limit that would still
  bite on constrained hardware, and OPEN-11 would come back the moment the probe
  count grew again. A gate that passes because the test rig was enlarged is not a
  gate.
- **Demand-page the stack.** This is the *right* long-term answer, but ADR-022
  explicitly defers demand paging, and adding it to the #PF path is a large
  change to make while a red gate is open. It stays deferred; see "Later".

## Decision

Reduce `USER_STACK_SIZE` from 8 MiB to **256 KiB** (64 frames per process).

```
per process   64 frames
29 processes  1,856 frames (~7 MiB)  vs  28,502 available
```

That is a ~16x margin instead of a 2x deficit.

**Everything ADR-021 binds is preserved:** the stack remains USER, RW+NX, it
still sits top-down below `USER_STACK_TOP` per the SysV AMD64 ABI, and the
unmapped guard page directly below it is unchanged — an overflow still faults
cleanly rather than corrupting anything. ADR-021's *permission* model is
untouched; only the size, which ADR-021 records in a layout table rather than
mandates, changes.

256 KiB is ample for this system's ring-3 programs: the deepest observed frame
is `sha256test`'s `sub $0x490,%rsp` (1,168 bytes).

## Later

When demand paging lands (ADR-022), restore a large *reserved* stack region and
populate it lazily. At that point this DDR is superseded — the point of the
8 MiB figure was headroom, and headroom is free once pages are faulted in on
demand rather than allocated up front.

## The rule this earns

**An eagerly-allocated per-process reservation is a divisor on the maximum
process count.** Any such reservation must be stated as a ceiling
(`free_frames / per_process`) and checked against the number of processes the
system actually spawns — a reservation that is merely "generous" is a limit
wearing a disguise, and it fails nondeterministically, in whichever unrelated
subsystem happens to allocate last.


## Refutation (added the same day, before any of this shipped)

**The change was implemented, measured, and reverted. It does not fix OPEN-11.**

With `USER_STACK_SIZE` at 256 KiB, `smoke-sha256` run three times gave
**PASS, FAIL, FAIL** — the identical `#GP` at pid=29 survived the fix.

The premise was false. The serial log reports:

```
free frames after release=0x0000000000006F53  (balanced)
```

The PMM is **nearly full and balanced**. Processes exit and release their frames,
so the ~29 spawned threads never coexist, and the "28,502 / 2,048 = ~13 process
ceiling" never applies. Memory was never exhausted. `AGENT_OOM_KILLED` comes from
a deliberate OOM *test* (`AETHER_SEC_OOM_OK` on the next line), not from real
pressure — though the garbage PID it prints is a genuine, separate bug.

Kept as a record so the arithmetic is not re-derived and re-believed. The
underlying inefficiency is real (an eager 8 MiB reservation is wasteful) but it
is **not** OPEN-11, and it must not be changed under that pretext.

The rule at the bottom of this DDR still stands on its own merits; it simply did
not apply here.
