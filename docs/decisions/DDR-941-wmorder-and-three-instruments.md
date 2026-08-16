= DDR-941 — `smoke-wmorder` (new, untracked) + three instruments for the open defects

**Status:** ACCEPTED. Instrumentation and one new gate record. **No fixes.**
**Date:** 2026-08-16
**Numbering note:** the wmorder record was planned as DDR-940, but DDR-940 was
already taken by "smoke-agent-click has two failure modes" (`de993b5`). Per
§0.4 the number was verified free in **both** `docs/ddr/` and `docs/decisions/`
before allocating 941.

## 1. `smoke-wmorder` — new, previously untracked

CI run 31928006000 (tip `6ea5b42`), shard 2, last gate of 23:

```
[wmorder] startup ordering: compositor must observe the 3-set before it shrinks...
[wmorder] FAIL — set reached 3 but never shrank; close path broken
```

Started 05:34:09, failed 05:36:09 — **exactly the 120 s window**, so it is a
timeout, not a fast assertion failure (§8: check elapsed against the window
before reading code).

The gate asserts the compositor sees `PRADYOS_ZORDER 0 1 2` and then observes
the set shrink (`PRADYOS_SURFACE_GONE`). The set reached 3, so surfaces were
created; it never shrank, so no surface closed.

**Hypothesis, explicitly NOT adopted:** the surface owner's thread never ran,
which would match DDR-940's mode A. §6.0-C forbids assuming it — mode A's
evidence must not be borrowed for a different gate. `smoke-wmorder` gets its
own root cause or it gets none. It is recorded here as an open, uncharacterised
intermittent with **one** observation and no hit rate.

## 2. Instruments shipped with this DDR

### (a) `rqmiss=` / `rqmst=` — the hole DDR-936 left

DDR-940 established that `ubcas=0 ubrq=0` on a failing run, excluding both
enqueue gates. The remaining strand path is in `rq_take` (`sched.c`):

```c
__atomic_store_n(&t->rq_on, 0, __ATOMIC_RELEASE);
if (t->state == THREAD_READY) return t;
/* else: entry is UNLINKED, rq_on cleared, and dropped — nothing re-enqueues it */
```

A thread dropped here is stranded permanently **while incrementing neither
`ubcas` nor `ubrq`** — precisely the hole left when those two read zero.
`rqmiss` counts these drops and `rqmst` records the state seen.

Cumulative, **not drained**: a single strand event is one increment in an
entire boot, and a drained counter would report it on one heartbeat and zero
forever after — which is exactly how it would be missed.

### (b) `PRADYOS_BTN_STATE` + `btnedge=` — DDR-940 mode B

The plan called for an unconditional print before
`down = ms.buttons && !prev_btn`. **That would have been harmful.**
`sys_mouse_poll` (`sys_input.c:39-45`) reads *current state* and returns 0
whenever the device exists — it is not an event queue — so that block runs on
every iteration of the compositor's main loop. An unconditional print there
emits thousands of lines per second, swamps the serial log, and slows the
compositor enough to plausibly mask the ~11% timing flake it is meant to catch.
Shipped **on-change only**, which keeps every transition and adds no spam.

That same fact exposes a mode-B mechanism a ring-3-only log cannot see: with a
state-based poll, **a press+release completing between two polls is invisible
to ring 3 by construction** — no edge-detector bug required. So the driver also
counts press edges it saw (`btnedge=`, cumulative).

Reading the pair:

- `btnedge` increments, no matching `PRADYOS_BTN_STATE` ⇒ the event reached the
  driver and was **coalesced** before ring 3 polled ⇒ structural, fix is an
  event queue or edge latch, not the compositor.
- `btnedge` does not increment ⇒ the event never reached the driver ⇒ below
  virtio-input (injector, transport, IRQ).
- both increment and the press still does nothing ⇒ the defect is in the
  dispatch, and DDR-937's widened dump names the branch.

### (c) `[boot-load] FAILED <name> reason=…` — anonymous probe failures

`user_boot_from_sfs` has ~30 call sites and **every one discards its return**.
Two of its failure paths were unusable: the `pmm_alloc_pages` failure returned
0 **silently**, and the ELF failure printed `rc=` with **no filename**. With 30
probes booting through one function, an unnamed failure cannot be told apart
from a probe that booted and then misbehaved — the same anonymity defect as
`sys_exit` without a pid (DDR-940). Both now name the file.

This is deliberately instrumented **in the function**, not at the wmorder call
site as planned: one edit covers all ~30 probes, and the wmorder question
("did SURFDEST.ELF ever load?") is answered as a side effect.

## Not doing

No fix for any of the three open defects. No fix for wmorder. §6.0-B, and the
tally that motivates it: DDR-920, 928, 932 each named a mechanism from
inference and each was refuted; DDR-935 shipped on one unrepresentative pass;
the "16 gates" claim was grep output mistaken for evidence; and DDR-936's own
two candidate gates were just excluded by measurement. Six for six against
guessing.

## Verification bar

Build warning-clean (`-Werror` clang + nasm); `sentinel_collision.sh` clean at
**160** (`PRADYOS_BTN_STATE` is the one new sentinel; `rqmiss`/`rqmst`/`btnedge`
are fields on the existing `[hb]` line and `[boot-load] FAILED` extends an
existing prefix); `ci-shard-check`, `ci-probe-rodata-check`,
`ci-start-align-check`; `smoke-blkmq`, `smoke-rqstress-liveness`; and
`smoke-agent-click` x10 to confirm the on-change print does not perturb the
mode-B hit rate.
