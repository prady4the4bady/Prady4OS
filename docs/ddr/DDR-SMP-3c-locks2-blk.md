# DDR-SMP-3c-locks-2 — atomic per-disk block serialization (ADR-030 stage 3c)

> DDR before code. Continues the full-3c prerequisite campaign: make the block
> layer's per-device serialization cross-CPU safe.

## The subtlety
`virtio_blk`'s `submit()` serializes with a `busy` flag **held across
`sched_block()`** — the requester sleeps while the device DMAs and the IRQ
handler wakes it. That is a **sleep-mutex**, not a spinlock: a spinlock held
across a block would deadlock every other CPU spinning on it. So the fix is
NOT "wrap submit in a spinlock" — it is making the `busy` acquire **atomic** so
two CPUs can't both observe `busy==0` and issue overlapping requests into the
one-in-flight virtqueue.

## Decisions
- **D1:** `busy` acquire becomes `__atomic_exchange_n(&v->busy, 1, ACQUIRE)` in
  a yield-loop: the loser sees 1 and yields (sleeps), exactly today's behavior
  on one CPU, now race-free across CPUs. Release is
  `__atomic_store_n(&v->busy, 0, RELEASE)`. The `cli/sti` stays (it also gates
  against this CPU's own IRQ handler touching `done/waiter`).
- **D2:** `v->done`/`v->waiter` are written by the IRQ handler and read/written
  under the busy holder; the busy mutex already serializes holders, and the IRQ
  fires on the owning CPU (INTx, BSP for now) — no extra lock needed this slice.
  When device IRQs move to the I/O APIC / other CPUs (DDR-714 stage C), the
  completion fields get their own review (noted).
- **D3:** No behavior change on the single CPU that does block I/O today; this
  is a correctness-under-future-concurrency change, so the existing FS/block
  gates are the regression surface — no new gate.

## Gate
None new. All FS gates (`smoke-fs`/`-rw`/`-sfs-rw`/`-ext4`) + `smoke-user`
(loads ELFs through the block path) exercise it every run; 58 gates unchanged.

## Non-goals
Multi-in-flight requests (request-tag table — needs MSI-X, DDR-714 C);
per-mount VFS locks (next locks slice); moving block IRQs off the BSP.
