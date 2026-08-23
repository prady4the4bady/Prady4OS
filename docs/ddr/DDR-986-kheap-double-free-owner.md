# DDR-986 — OPEN-13: name the double-free's call sites, and a correction on what it costs

**Status:** DESIGN. Instrument only — no fix, because OPEN-13 has no named mechanism.
**Date:** 2026-08-23
**Relates to:** DDR-980 §2 (the single capture), OPEN-13.
**Corrects:** CLAUDE.md §OPEN ISSUES OPEN-13, on two points (§2, §3).

## 1. What the one capture gives us

`[kheap] double-free ptr=… objsize=0x80` -> `*** KHEAP PANIC: kfree: double free ***`
at t≈247, `smoke-blkmq-trace`, shard 4, on a **docs-only** commit — so not a
regression. `KHEAP_DEBUG` is unconditionally `1` (`kheap.c:20`), so this
detector is live in the SHIPPED kernel, not a debug build.

`objsize=0x80` is 128, and `size_classes[] = {16,32,64,128,256,512,1024,2048}`
(`kheap.c:53`). The dedicated caches are pcb=512, cap=16, ipc=256 — none is 128.
So 128 is a **generic** class and the size->structure mapping genuinely does not
resolve: any `kmalloc(65..128)` qualifies. CLAUDE.md is right about that.

## 2. Correction 1 — the missing datum is the FIRST free, not the allocation

CLAUDE.md prescribes recording "alloc/free return addresses per object". For a
double free the allocation site is the *least* useful of the three. The panic
already stands at the second free, and `__builtin_return_address(0)` there is
free. What is unrecoverable after the fact is **where the first free came
from** — and that, paired with the second, names the racing pair directly.

So the instrument records exactly one new thing: the return address of the free
that put the object on the free list.

## 3. Correction 2 — this does not meaningfully touch the hot path

CLAUDE.md defers the instrument because "that touches a hot allocator path, so
make it opt-in". That premise does not survive reading `cache_free`
(`kheap.c:129`), which **already** does, unconditionally under `KHEAP_DEBUG`:

```c
for (struct free_obj *f = s->free; f; f = f->next)   /* O(free_count) */
    if (f == (struct free_obj *)ptr) { ...panic... }
memset(ptr, POISON_FREE, c->obj_size);               /* O(obj_size) */
```

A linked-list walk of up to `objs_per_slab` entries (31 for the 128 class, 255
for the 16 class) plus a 128-byte `memset`, on **every** `kfree`. Adding one
8-byte store is not a hot-path change against that; it is noise. The store also
lands inside memory the `memset` just wrote, so it costs no extra cache line.

**This matters for the outcome, not just for tidiness.** OPEN-13 has been seen
exactly once, in CI, never locally. An opt-in instrument that is off in CI can
never capture it. Making it opt-in would guarantee the instrument never fires —
which is why the premise is worth correcting rather than working around.

Therefore: **on whenever `KHEAP_DEBUG` is on**, i.e. shipped, like the detector
it extends.

## 4. Where the byte goes

Object layout while free (`kheap.c:32`, `:102`, `:153`):

| offset | contents |
|---|---|
| 0..7 | `struct free_obj.next` |
| 8..15 | `KHEAP_CANARY` |
| 16..23 | **unused** — currently `POISON_FREE` |

So the freeing return address goes at offset 16, written after the `memset` and
beside the canary re-arm. No new global, no new lock, no allocation: it reuses
bytes the free path already dirties. (§DDR-826 concerns writable globals in
probe ELFs; this is kernel text, and adds none.)

**The gap, stated rather than hidden:** this needs `obj_size >= 24`, so the
16-byte class and the dedicated `cap` cache (also 16) are excluded and keep
today's behaviour. OPEN-13 is the 128 class, so the one case on record is
covered. A 16-byte double free would still print only `ptr=`/`objsize=`.

## 5. Output

Extend the existing block — same line, two new fields:

```
[kheap] double-free ptr=0x… objsize=0x80 freed_by=0x… now_by=0x…
```

`freed_by` = the recorded first free. `now_by` = `__builtin_return_address(0)`
in `cache_free`.

**Reading it (§NON-NEGOTIABLE 18).** These are kernel text addresses, and every
build of this kernel loads at the same base, so an address alone does not
identify the build. Resolve both against the exact `build/kernel.bin` that
produced the log, whose hash the gate records (R1) — not against a fresh local
build. This session already made that mistake once, resolving a RIP against the
default build when the gate ran `BSP_LIVENESS=1`, where `timer_tick` moves
`0x9be0`->`0x9c10`.

## 6. What would make this a finding

The instrument is not a fix and must not be reported as one. It converts the
next OPEN-13 occurrence from "some 128-byte object, somewhere" into two named
call sites. Only then does §NON-NEGOTIABLE 3 permit a fix.

Two outcomes are worth predicting now, so the reading is not motivated later:
- `freed_by == now_by` -> one site freeing twice: a missing NULL-after-free or a
  retry path re-entering.
- `freed_by != now_by` -> two owners believe they own the object: a refcount or
  handoff defect, and the pair names both halves.

**Not claimed:** that OPEN-13 is an SMP race. The capture is a single event on
one shard; `kfree` takes `g_heap_lock` (`spin_lock_irqsave`), so the free list
itself is serialised. Whatever double-frees is above the allocator, not in it.
