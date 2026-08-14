= DDR-886 — a probe verdict must distinguish LATE from WRONG

**Status:** ACCEPTED — generalises DDR-885 to the two virtio-blk probes and
covers the `[boot-load]` instrument landed in e49a23f.
**Date:** 2026-08-15
**Lineage:** DDR-910 (observe, don't assume) → DDR-885 (rqstress) →
**DDR-886 (this)**. Related: DDR-785 (a foreign probe FAIL fails the booting
gate), DDR-878 (blk waiter FIFO), DDR-775/776 (blk completion waits).

## Evidence

CI run 31834006700, shard 1, on a kernel carrying the `[boot-load]` instrument:

```
PASS smoke-rqstress-liveness (180s)          <- item 47 did not hit this run
...
[smoke] FAIL — a probe reported 'blk integrity FAIL' during this gate's boot.
[smoke]   [boot-load] PRISM.ELF t=3029
[smoke]   [boot-stamp] B proofs-begin t=3093
[smoke]   [blk] multi-inflight FAIL
[smoke]   [smp] blk integrity FAIL
shard 1: FAILED at smoke-winops after 12 of 36 gates
```

`smoke-winops` has nothing to do with block I/O; per DDR-785 it failed because a
foreign probe reported FAIL during its boot. Both blk probes failed together.

## The defect — same shape as DDR-885, twice

**`blkmq_proof` (main.c:632)**

```c
uint64_t dl = g_ticks + 200;                       /* fixed 2 s */
while ((g_mq_done & 3u) != 3u && !(g_mq_done >> 8) && g_ticks < dl)
    yield();
kputs((g_mq_done & 3u) == 3u ? "OK" : "FAIL");
```

**`smp_blk_integrity` (main.c:673)**

```c
uint64_t dl = g_ticks + 400;                       /* fixed 4 s */
while ((g_blkint_done & 0xfu) != 0xfu && g_ticks < dl)
    yield();
kputs(((g_blkint_done & 0xfu) == 0xfu && !(g_blkint_done >> 8)) ? "OK" : "FAIL");
```

Both wait on a **fixed deadline**, then fall through and read the result
immediately. A worker that has not finished yet is indistinguishable from a
worker that read the wrong bytes. On a loaded 4-vCPU TCG runner, four threads
each doing sector reads can easily exceed 2 s / 4 s without anything being
wrong.

Worse, `[smp] blk integrity FAIL` is emitted from **three** sites with the same
text — no block device or no page (`:676`), the single-threaded reference read
failing (`:684`), and the verdict (`:694`) — so the message cannot even tell a
resource shortage from a data mismatch.

The bitmask already encodes the distinction and is thrown away: bit `id` means
worker *id* succeeded, bit `id+8` means it reported a mismatch. "Not all low
bits set" is LATE; "any high bit set" is WRONG. These are opposite conclusions.

## Decision

1. **Drain before verdict.** After the pacing deadline, wait again — bounded —
   for the workers to land, exactly as DDR-885 did for rqstress. The pacing
   deadline keeps the reads overlapping (the point of the proof); it must not
   decide the verdict.
2. **Report the bitmask and the reason.** Emit
   `[blk] multi-inflight FAIL done=0x%x` and
   `[smp] blk integrity FAIL <reason> done=0x%x`, so a failure says whether
   workers were late (low bits unset) or a checksum mismatched (high bits set),
   and which of the three integrity sites fired.
3. **Do not change the assertions.** All workers must still complete and all
   checksums must still match. Only the ability to tell late from wrong is added.

### Bounds are derived, not chosen

Each drain gets the same budget its pacing loop already had (200 ticks for
`blkmq_proof`, 400 for `smp_blk_integrity`), so each probe's worst case at most
doubles — 4 s and 8 s respectively at the 100 Hz PIT, both far inside every
consuming gate's `TIMEOUT_S` (tightest is 90 s among the gates these run under).
S2 holds: still bounded, still terminates.

## Why this is not a blind fix

§6 forbids fixing on the unconfirmed virtio-blk hypothesis. This DDR does not
fix virtio-blk. It makes the probes *report what they observed*, which is the
precondition for confirming or refuting that hypothesis at all. If the next
failure prints `done=0xf` with a high bit, the blk layer is genuinely returning
bad data (DDR-775/776 territory). If it prints low bits unset, the reads were
merely slow and there is no data defect. Today both print the same word.

## Also recorded here

`e49a23f` added `[boot-load] <FNAME> t=<ticks>` on entry to every
`user_boot_from_sfs`, and cited "DDR-886" before this file existed. That
instrument belongs to this DDR: same principle, applied to the boot sequence —
a stuck boot must name the load it stopped on rather than going silent. It is
confirmed working in this run (`[boot-load] PRISM.ELF t=3029`).
