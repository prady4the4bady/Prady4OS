# DDR-BLK-1 — multi-in-flight virtio-blk requests

> DDR before code. The payoff DDR-714 C1–C3 unblocked: with per-device MSI-X
> and a cross-CPU-safe completion path, the one-request-at-a-time `busy`
> sleep-mutex is the last serializer. Multiple threads (on multiple CPUs) can
> now have requests outstanding on one disk concurrently.

## Decisions
- **D1 — per-request slots.** Each disk gets `VBLK_NREQ` (8) request slots in
  its existing `reqbuf` page (each slot: 16-byte header + status byte, 32-byte
  stride). Per-slot `done` flag + `waiter`. A `head2slot[]` array (vq-size)
  maps the used-ring's returned descriptor head back to its slot.
- **D2 — the vq goes under `compl_lock`.** With concurrent submitters,
  `virtq_add/publish` (CPU A/B) and `pop_used/free_chain` (the vector's CPU)
  genuinely interleave — all virtq mutation + slot alloc/free moves under the
  per-device `compl_lock` (short, non-sleeping critical sections; the C3
  analysis's "no vq lock needed" rested on one-in-flight, which this removes).
- **D3 — slot exhaustion sleeps, never spins.** A submitter finding no free
  slot records itself as `slot_waiter` and `sched_block_on(&compl_lock)`; the
  completion handler wakes it after freeing a slot (same lost-wakeup-safe
  pattern as request completion).
- **D4 — the `busy` sleep-mutex is deleted** (locks-2's atomic acquire is
  subsumed: exclusion is now the lock + slot ownership). `submit()` blocks
  only its own request's completion; other threads' requests proceed.

## Gate
`smoke-blkmq` (`-smp 4`): a boot proof spawns two kernel threads issuing
interleaved reads of DIFFERENT sectors on one disk simultaneously (each
verifies its own data pattern); prints `[blk] multi-inflight OK` when both
round-trips complete correctly. Plus the whole FS family re-verifies
correctness under the new path. 63 gates.

## Non-goals
Multi-queue (one vq per disk stands); request coalescing/elevator; async
(fire-and-forget) kernel API — callers still block on their own request;
per-queue MSI-X vectors.
