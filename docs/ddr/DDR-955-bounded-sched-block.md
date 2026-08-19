# DDR-955 — `sched_block_timeout`: make every kernel wait bounded

Status: DESIGN ACCEPTED. Implementation NOT started (this DDR precedes the code,
per R16 / CLAUDE.md §3 "ADR/DDR before the code it governs").

## a. The defect class

`while (!done) sched_block_on(lk);` is an **unbounded** wait: if the wakeup is
ever lost or the producer dies, the thread blocks forever and its resources are
never reclaimed. Every such site is an S2 violation.

Live sites (`grep -rn "sched_block_on" kernel/`), all currently unbounded:

| site | waits for | proposed deadline |
|---|---|---|
| `virtio_blk.c:222` | a free request slot | 500 ticks (5 s) |
| `virtio_blk.c:260` | request completion | 500 ticks (5 s) |
| `ipc/ipc.c:60` | IPC endpoint message | 100 ticks (1 s) |
| `ipc/bcast.c:70` | broadcast subscriber | 100 ticks (1 s) |

`sched_block_on` itself is `sched.c:1304`.

## b. Answers to the §5 architecture checklist

1. **New NSI/syscalls:** none. Verified live max = **94**; nothing is consumed.
2. **TCB extension:** one field, `uint64_t block_deadline` (0 = no deadline).
   **`kmalloc` does not zero** (CLAUDE.md §0.6), so it MUST be explicitly
   initialised to 0 in `sched_create` alongside every other TCB field — this is
   the exact failure mode recorded in memory node `tcb-fields-not-zeroed`.
3. **PMM/VMM:** unaffected. No shared mapping, no new PTE class.
4. **Capability:** none. This is a kernel-internal primitive with no ring-3
   surface.
5. **AETHER queue/audit:** no new record type.
6. **Scheduler hooks:** `sched_create` (init the field), `timer_tick` (expiry
   scan), `sched_unblock` (reuse as-is).
7. **Filesystem:** none directly; `virtio_blk` is the first consumer, so every
   FS gate is the regression surface.
8. **Network policy:** none.
9. **Compositor/UI:** none.
10. **Gate determinism:** see §d — the gate must not depend on TCG timing
    (DDR-735/771 lesson).
11. **Security invariants:** S2 (bounded waits) is the one this *fixes*. No
    other invariant is touched; a timeout cannot grant authority.

## c. Two design corrections to the prescribed plan

**1. The expiry scan's lock is wrong as specified.** The instruction says the
timer-tick scan "runs under the per-CPU rq lock, not `g_sched_lock`, so no
inversion". But a blocked thread is **not on a runqueue** — that is what blocked
means. There is no per-CPU rq blocked list to walk under the rq lock. The scan
must therefore walk the thread table under `g_sched_lock`, which is the correct
inner lock and is what `sched_unblock` already takes. Walking a list that does
not exist would either not compile or silently scan nothing — a gate that then
"passes" would be measuring nothing (the R3/S18 class).

**2. The return-value contract is racy as specified.** "On wake: if
`g_ticks >= block_deadline` before done is set, return `-ETIMEDOUT`" makes the
verdict a *re-read* of two globals after waking — the same defect DDR-952 fixed
in `timer_tick`, where consumers re-read `g_ticks` instead of using the value
the tick produced. A completion that lands in the same tick as the deadline
would be reported as a timeout and the buffer discarded, corrupting a request
that actually succeeded. Instead the waker must record **why** the thread woke:
the expiry path sets a `blocked_timed_out` flag on the TCB before calling
`sched_unblock`, and `sched_block_timeout` returns based on that flag, not on a
clock comparison. One writer, one reader, no re-read.

## d. Gate design — `smoke-blk-timeout`

Three arms (R8, all discriminating):

- **A (timeout fires):** a test hook suppresses `req->done`. Assert
  `-ETIMEDOUT` is returned and `-EIO` propagates. Assert the kernel is still
  alive afterwards (a bounded wait that panics is not a fix).
- **B (normal path unaffected):** an ordinary read completes and returns 0.
  This is the arm that catches a deadline that is too aggressive.
- **C (no false timeout under load):** run the arm-B read while the block layer
  is contended; assert zero `[vblk] timeout` lines.

Determinism: arms assert on **sentinels and tick counts read from the guest**,
never on host wall-clock, because TCG timing is not reproducible.

Acceptance: `gate_rate.sh smoke-blk-timeout 20` = 20/20, plus
`fs_regression.sh` 9/9 unchanged — `virtio_blk` is under every FS gate, so a
regression there would surface as filesystem failures, not block-layer ones.

## e. Relationship to BUG-A (the SMP stall)

If the stall is a thread blocked forever on a lost `virtio-blk` wakeup, this
primitive converts it from a silent hang into a loud, attributable `-EIO` with a
named device and LBA. That is **diagnostic value, not proof of a fix** — and it
must not be reported as closing BUG-A. BUG-A is closed only by the DDR-777
discriminator naming the mechanism, per CLAUDE.md §0.2.

---

## j. CORRECTIONS 3-6 — verified against the tree before implementation (R18)

Implementation has NOT started. These are defects in the prescribed design that
would have compiled and then deadlocked or silently done nothing.

### Correction 3 — `g_all_threads[]` / `SCHED_MAX_THREADS` do not exist
Neither symbol is in the tree. Threads are reachable via per-CPU runqueues
(`g_rq[c]`, `rq_next`, **READY only** — a BLOCKED thread with an expired
deadline is unreachable there) and a ring linked by `tcb->next`. `g_sched_lock`
(`sched.c:71`) "covers ring TOPOLOGY only" (`sched.c:637`).

### Correction 4 — the calling convention is INVERTED in the prescription
The prescribed body opens with `spin_acquire(lk)`. That is backwards and would
**double-acquire and deadlock on the first call**. The proven contract, from
`sched_block_on` (`sched.c:1304`) and its live caller (`virtio_blk.c:259-260`):

```c
/* caller ALREADY holds lk (spin_lock_irqsave earlier) */
while (!v->req[s].done)
    sched_block_on(&v->compl_lock);
```

`sched_block_on` therefore **receives** the lock held, does
`spin_unlock(lk); schedule(); spin_lock(lk);` and **returns with lk still held**.
`sched_block_timeout` MUST match this exactly, or every caller breaks: they
proceed to `spin_unlock_irqrestore(&v->compl_lock, fl)` (`virtio_blk.c:266`)
on a lock the function had already released.

### Correction 5 — three wrong symbol names
| prescribed | actual in tree |
|---|---|
| `spin_acquire` / `spin_release` | `spin_lock` / `spin_unlock` |
| `this_cpu()->current` | `current_thread` |
| `yield()` | `schedule()` |

### Correction 6 — the expiry scan is CASE A, and simpler than either branch
`sched_unblock` (`sched.c:1322`) does **not** take `g_sched_lock`. It performs an
atomic CAS `BLOCKED -> READY` then `rq_push`, which is documented as "a leaf
lock, **safe from IRQ handlers** and under device compl locks". So the timer may
call `sched_unblock` directly — neither the `g_timeout_lock` list of CASE B nor
a deferred wake array is needed.

`schedule()` also no longer takes `g_sched_lock` at all (`sched.c:634-637`), and
`g_sched_lock` is always taken with `spin_lock_irqsave` (`sched.c:628`), so IRQs
are masked while it is held and a timer IRQ cannot re-enter it on the same CPU.

**Remaining open question — settle before coding:** whether the `tcb->next` ring
enumerates every thread or only the per-CPU idles (`g_idle` is
`struct tcb *g_idle[PERCPU_MAX]`, `sched.c:77`). If it is idle-only, the scan
reaches no blocked thread and the timeout **silently never fires** — a bounded
wait that is not bounded, passing any gate that only tests the happy path. Read
`sched_snapshot` (which walks threads under `g_sched_lock`) to find the true
enumeration head before writing the scan.

## k. Design RESOLVED — the two open questions, answered from the tree

### The `->next` ring DOES enumerate every thread (Correction 3 closed)
`sched_snapshot` (`sched.c:1077-1099`) walks `t = current_thread; ... t = t->next`
and reports `out->state = t->state` for each entry — which is how `ps` displays
BLOCKED processes. So the ring reaches blocked threads, and the §j worry that it
might be idle-only is **refuted**. The expiry scan can use it.

Ring head: **`current_thread`**. The ring is circular, so any live member is a
valid start; `sched_snapshot` uses exactly this and terminates on
`t != <start>`. There is no dedicated `g_thread_head` symbol — do not invent one.

### Locking: CASE A, and the protection is `irq_save()`, not `g_sched_lock`
- `sched_tick` (`sched.c:1247`) contains **zero** references to `g_sched_lock`.
- `sched_snapshot` guards its ring walk with `irq_save()` (`sched.c:1080`), NOT
  `g_sched_lock`. (Any note claiming it walks under `g_sched_lock` is wrong.)
- `sched_tick` already runs in the timer IRQ with interrupts masked, so it has
  the same protection `sched_snapshot` relies on.
- `g_sched_lock` "covers ring TOPOLOGY only" (`sched.c:637`) and is always taken
  with `spin_lock_irqsave` (`sched.c:628`), so IRQs are masked while it is held
  and a timer IRQ cannot re-enter it on the same CPU. Taking it in the scan is
  therefore **safe but optional**; take it for SMP topology safety, since another
  CPU may be splicing the ring while this CPU walks it.
- `sched_unblock` does not take `g_sched_lock` (§j Correction 6), so the
  collect-then-wake pattern keeps lock order `g_sched_lock -> rq` (R9) with no
  nesting: collect under the lock, release, then wake.

### Settled implementation shape
```
sched_tick():  ... after the tick is taken ...
    fl = spin_lock_irqsave(&g_sched_lock);
    t = current_thread;
    do { if (t->state == THREAD_BLOCKED && t->block_deadline &&
             g_ticks >= t->block_deadline && n < 32) {
             t->wake_timed_out = 1; t->block_deadline = 0; wake[n++] = t; }
         t = t->next; } while (t != current_thread);
    spin_unlock_irqrestore(&g_sched_lock, fl);
    for (i = 0; i < n; i++) sched_unblock(wake[i]);
```
`sched_block_timeout` mirrors `sched_block_on` exactly: **called with `*lk` held,
returns with `*lk` held** (§j Correction 4). Only additions are setting
`block_deadline` before the block and reading `wake_timed_out` after.

**Nothing above is implemented yet.** This section removes every unknown; the
next session writes code, not analysis.
