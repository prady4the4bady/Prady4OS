# DDR-830 — a double free is silently accepted, putting one frame on the free list twice

**Status:** accepted
**Date:** 2026-08-04
**Governs:** `kernel/mm/pmm.c` `pmm_free_pages`
**Relates to:** OPEN-11 — but see "Measured outcome": this does NOT fix it
**Family:** the recurring structural defect — **instance 8**

## The evidence that led here

`smoke-sha256` fails ~1 run in 3. With the thread name and the executing bytes
added to the trap line (`kernel/idt.c`), a failing run shows:

```
#PF pid=29 name=SHA256 rip=0x800000009E err=0x0 cr2=0x0
    bytes@rip= 65 6C 73 7A 81 88 8F 96 ...
    page+0   = 13 1A 21 28 2F 36 3D 44 ...
```

Those are not instructions. They are the arithmetic sequence **7n+3**, running
**continuously across the whole page**: index 112 sits at page offset 0, and at
offset 158 the byte is `0x65` = index 270 = 112+158. Consistent.

The only producer of a continuous `7n+3` stream with an arbitrary start index is
`user/bigwritetest.c:44`, which builds an 8 KiB pattern **on its stack**.
(`kernel/main.c:765` also writes `7n+3`, but into a *page-aligned* 512-byte
buffer, so it would place index 0 at offset 0. It is excluded by the offset.)

**So `SHA256`'s text frame and `bigwritetest`'s stack frame are the same physical
frame.** Two address spaces, one page.

## The defect

```c
void pmm_free_pages(uint64_t addr, unsigned order) {
    if (order == 0 && pmm_refcount) {
        uint64_t idx = addr >> PAGE_SHIFT;
        if (idx < PMM_NFRAMES && pmm_refcount[idx] > 1) {   /* shared: deref only */
            pmm_refcount[idx]--;
            return;
        }
    }
    rc_set(addr, order, 0);
    ... push onto the free list ...
}
```

The refcount is tested for `> 1` and nothing else. **`refcount == 0` — a frame
that is already free — takes the same path as `refcount == 1`.** A second free of
an already-free frame therefore pushes it onto the free list a *second* time, and
the allocator later hands the identical frame to two independent callers.

Nothing complains. The first symptom is an unrelated process executing another
process's data, in a different subsystem, one run in three.

## Decision

`pmm_free_pages` must **reject** a free of a frame whose refcount is already 0,
loudly, naming the address and the caller's return address — never silently
re-add it to the free list.

A double free is a kernel bug by definition; there is no legitimate caller. The
guard turns silent, delayed, cross-subsystem corruption into an immediate report
that identifies the offending path on the spot.

## Why this shape and not another

- **Not** "make the allocator tolerate it". Tolerating a double free is what
  produced OPEN-11: the free list is a set, and a set that accepts the same
  element twice stops being one.
- **Not** a poison/checksum sweep. That detects the damage later, somewhere else
  — precisely the failure mode we are trying to end.
- The guard is the same remedy applied in DDR-823/824 to `syscall_register` and
  `check_global_forbidden`: **when a check discards or silently accepts invalid
  input, the discard must be made loud.**

## The rule this earns (eighth instance)

**A free list is a set. Any operation that can insert a duplicate must reject the
duplicate, not absorb it.** The cost of absorbing it is not paid by the buggy
caller — it is paid, much later and nondeterministically, by whichever unrelated
subsystem is handed the aliased resource.

## Measured outcome — HARDENING ONLY, NOT THE OPEN-11 FIX

The guard was implemented and the gate re-run. **It fired zero times, and the
corruption still occurred.** So the frame aliasing is real but is NOT produced by
a double free.

This DDR therefore stands on its own merits — `pmm_free_pages` genuinely did
treat an already-free frame as freeable, and that hole is now closed and loud —
but it must **not** be recorded as OPEN-11's fix, and OPEN-11 stays open.

Further evidence gathered with the guard in place: the aliased pattern's start
index differs every run (112, then 160), i.e. a *different* page of
`bigwritetest`'s stack aliases the probe text each time. That points at the
mapping/page-table path rather than at frame accounting.

## Follow-up (tracked, not silently deferred)

The guard reports *where* the double free happens; the offending caller must then
be fixed on its own merits. Until that caller is identified and repaired, this
DDR closes the **amplifier**, not the source. OPEN-11 stays open until both are
done and `smoke-sha256` passes 20/20.
