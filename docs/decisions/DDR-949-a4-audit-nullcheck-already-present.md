= DDR-949 — A4 audit: the NULL-check already exists; two other sites did not

**Status:** ACCEPTED. Defensive hardening + audit record. **Not a workers-late fix.**
**Date:** 2026-08-16
**Numbering:** 949 verified free in **both** `docs/ddr/` and `docs/decisions/` (§0.4).

## R20 contradiction record #1 — the requested change is already in the tree

Directive v4 §4 A4 asks to "add NULL-check + KASSERT in `blkmq_proof` and
`smp_blk_integrity`". **Both already have it**, added by DDR-934/939:

```c
kernel/main.c:682-683   if (sched_create(blkmq_reader,  (void *)N, "mqN")) mq_spawned++;
kernel/main.c:765-768   if (sched_create(blkint_worker, (void *)N, "biN")) bi_spawned++;
```

The return value is checked and counted; that counting is what produced
`spawned=4/4` and `spawned=24/24`. Re-adding the check would be a duplicate, and
adding a `KASSERT` on top would convert a **measured non-event** into a panic
risk: `spawned=4/4` means the NULL branch has never been taken on any observed
failure. Not added.

## R20 contradiction record #2 — the stated root cause remains refuted

v4 §2 F8 already concedes this, and it is restated here so the DDR record is
self-contained: **`sched_create` NULL return is NOT the cause of workers-late.**
DDR-942 measured `spawned=4/4` on the failing run — all four TCBs were created —
and `done=0x0` showed the workers never reached even their own failure path.

Per R21, nothing in this slice claims to fix workers-late. **The workers-late
root cause is OPEN.**

## What the audit DID find — two genuinely unchecked spawns

Auditing every `sched_create` call site in `kernel/` found exactly three
unchecked ones; two are real gaps and are fixed here:

| site | thread | consequence of a silent NULL |
|---|---|---|
| `main.c:2317` | `bench_partner` | `bench_ctx_switch()` then measures a context switch against a partner that **does not exist** and prints the result as a real number — a false performance measurement, which §0.7 exists to prevent |
| `main.c:2327` | `blk_test_thread` | the blk self-test silently never runs; its absence reads as "no output" rather than "failed to start" |

Both now report the failure by name. This is the **silent-drop defect class**
(BUILD_TRACKER §4, 16 instances): a failure that produces no output is
indistinguishable from a success that produces no output.

The `bench` one is the more serious of the two — it does not merely lose a test,
it **fabricates a measurement**, which is exactly the failure mode §0.7 (totals
need denominators) and Q8 (instrument validity) are written against.

## Scope

Defensive hardening only. No behaviour change on any path where
`sched_create` succeeds — which, per `spawned=4/4`, is every path measured so
far. This cannot fix workers-late and does not claim to.

## Still open, unchanged by this DDR

- **workers-late** (`done=0x0`, `spawned=4/4`) — root cause OPEN. Needs the
  per-worker instrument v4 §3 D4 describes: per-worker completion state plus
  `on_cpu`/runnability at the moment the workers are expected to run.
- **A1** (`writes=` live, DDR-948), **A2** (mechanism open, DDR-947),
  **B#3** (read existing `[boot-stamp]` A/B gap), **PMM** (DDR-943, needs a
  `pmmfree=` capture at the failure).
