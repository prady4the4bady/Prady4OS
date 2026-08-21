# DDR-961 — bounded `ipc_recv` and `bcast_wait`

Status: ACCEPTED. Written before the code it governs (R16).
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§0.4).
Completes the half of DDR-955 that was deliberately shipped as partial.

## 1. What DDR-955 left open, and why

DDR-955 shipped `sched_block_timeout` and bounded virtio-blk's two wait sites at
500 ticks. It converted `ipc_recv` and `bcast_wait` at 100 ticks, measured
**19/20** with **two lwIP `#GP` panics**, and reverted them. Its stated reason
was a return-value contract defect at each site, recorded in the code:

- `ipc_recv` (`ipc.c:62`) — "`-1` already means *cap denied*, so a timeout
  returning `-1` is indistinguishable from a permission failure."
- `bcast_wait` (`bcast.c:74`) — "returns `void` and fills `*out`, so an early
  timeout return would hand the caller an UNFILLED buffer with no way to detect
  it."

Both are real and both are fixed below. **Neither explains the panics**, and
that discrepancy is the substance of this DDR.

## 2. The panic explanation in DDR-955 does not survive inspection

DDR-955 attributes the two `#GP`s — `tcp_output+0x349` and
`tcp_process_refused_data+0x21`, `RAX=0xF000FF53F000FF53` (BIOS ROM filler read
as data) — to "a corrupt bcast buffer".

Trace it: `struct bcast_event` is `{ uint32_t type; uint64_t payload; }`, and
**every** caller (`main.c:277`, `main.c:292`) does nothing with it but
`kputs`/`kputhex`. An unfilled event prints garbage hex. There is no path from
those two call sites into lwIP's heap. The stated cause cannot produce the
observed symptom.

### The mechanism that does fit — a stale waiter pointer

Both wait sites register the current thread as *the* waiter before sleeping, and
rely on the **waker** to clear that registration:

```c
/* bcast_publish, bcast.c:57-61 */         /* ipc_send, ipc.c:45-49 */
if (s->waiter) {                            if (e->waiting_receiver) {
    struct tcb *w = s->waiter;                  struct tcb *r = e->waiting_receiver;
    s->waiter = 0;            /* <-- */         e->waiting_receiver = 0;   /* <-- */
    sched_unblock(w);                           sched_unblock(r);
}                                           }
```

On a **timeout** there is no waker, so nothing clears it. The thread returns
from `bcast_wait`/`ipc_recv` with `s->waiter` / `e->waiting_receiver` still
pointing at its own TCB, and in every one of the three call sites the thread
then **runs to completion and exits**. Its TCB is freed and the memory recycled.

A later `bcast_publish` or `ipc_send` — and the demo publisher does publish
after the subscribers — reads that dangling pointer and calls `sched_unblock()`
on freed memory, which writes a state field into whatever now owns that
allocation. lwIP's PCB pool is the largest consumer of that heap. A wild write
into a TCP PCB, followed by `tcp_output` dereferencing a pointer field that now
holds unrelated bytes, is exactly a `#GP` with a nonsense `RAX`.

That is a use-after-free introduced *by the timeout conversion itself*, not a
consequence of the return-type defects, and it is the reason a threshold change
could never have fixed it. **Clearing the waiter registration on the timeout
path is the load-bearing part of this change**; the signature fixes are what
make the timeout reportable.

This supersedes DDR-955's causal claim. Its *decision* to revert was right; its
explanation was not.

## 3. Design

### 3a. `ipc_recv` — a distinct return code, `-1` unchanged

```c
int ipc_recv(struct cap_table *caps, cap_t h, struct ipc_endpoint *e, uint64_t *out);
/*  0          message received, *out filled
 * -1          capability denied            (UNCHANGED — existing callers rely on it)
 * -ETIMEDOUT  deadline expired, *out untouched   (new)
 */
```

`ETIMEDOUT` is 110 (`kernel/include/errno.h:32`), already added by DDR-955 and
already used by `sched_block_timeout` and virtio-blk. No new errno.

Keeping `-1` as "cap denied" is what makes this safe to land: the one existing
caller's `== 0` test keeps its exact meaning, and the new value is a value it
never saw before.

### 3b. `bcast_wait` — `void` → `int`

```c
int bcast_wait(struct bcast_subscriber *s, struct bcast_event *out);
/*  0          event dequeued into *out
 * -ETIMEDOUT  deadline expired, *out untouched
 */
```

The compiler enforces nothing about an ignored return value, so the audit in §4
is what makes this real — not the signature.

### 3c. Both sites — clear the registration before returning (§2)

`sched_block_timeout` returns **with the lock held** (same contract as
`sched_block_on`), so the clear happens under exactly the lock that guards the
field:

```c
if (sched_block_timeout(&s->lock, &s->pending, BCAST_WAIT_TICKS) == -ETIMEDOUT) {
    if (s->waiter == current_thread)   /* a publish may have won the race and */
        s->waiter = 0;                 /* already cleared it — do not clobber */
    spin_unlock_irqrestore(&s->lock, flags);
    return -ETIMEDOUT;
}
```

The `== current_thread` guard matters: `sched_block_timeout` can return
`-ETIMEDOUT` on the same pass a publisher cleared `s->waiter` and unblocked us,
and an unconditional `s->waiter = 0` would then erase a *different* thread's
registration and lose its wakeup. `ipc_recv` takes the identical shape against
`e->waiting_receiver`.

### 3d. The `done` flag

`sched_block_timeout(lk, done, ticks)` short-circuits on `*done`
(`sched.c:1359`). DDR-955 kept `bcast_subscriber.pending` for this; it is
maintained by `enqueue()` and cleared in `bcast_wait` when the queue drains.
`ipc_endpoint.full` is the equivalent for IPC. Both are the same condition the
`while` loop already tests, so passing them adds no new state.

### 3e. Deadline

**500 ticks**, matching the two virtio-blk sites DDR-955 shipped green, not the
100 it measured red. 100 was never shown to be too short — the revert was for
the §2 defect — but there is no reason to pick the untested value when a tested
one exists, and a longer deadline strictly reduces the chance of a spurious
timeout in the demo threads whose sentinels the gates assert on.

## 4. Caller audit — complete, three sites, all in `kernel/main.c`

`grep -rn` over `kernel/` for each symbol. This is the whole blast radius:

| site | function | current | after |
|---|---|---|---|
| `main.c:166` | `ipc_receiver_thread` | `if (ipc_recv(...) == 0)` … `else "[recv] DENIED"` | three-way: `0` → print, `-ETIMEDOUT` → `"[recv] TIMEOUT"`, else `"[recv] DENIED"` |
| `main.c:277` | `sub_approvals_thread` | `bcast_wait(&sub_a, &e);` | check; on timeout print `"[sub-approve] TIMEOUT"` and stop looping |
| `main.c:292` | `sub_alerts_thread` | `bcast_wait(&sub_b, &e);` | check; on timeout print `"[sub-alert] TIMEOUT"` |

None silently falls through — the §4 requirement. Each timeout is *reported on
the serial console*, which means a timeout cannot pass a gate quietly: the
gates assert on `[recv] received:`, `[sub-approve] event type=` and
`[sub-alert] event type=`, and a timeout prints a different string, so the
affected gate goes red. That is the desired outcome; a bounded wait that hides
its own expiry is worse than an unbounded one.

The new strings are deliberately **not** added to any gate's sentinel list.
They exist to make a timeout legible in the log, not to be asserted on.

## 5. Verification — zero panics, not a pass rate. MEASURED.

`smoke-smpuser` **N=20**, kernel `26effe65daa046ff7c9257a6d5a1f423` (R1).
The criterion is **zero** panics across all 20 logs, not 20/20 green: DDR-955's
attempt was *19/20* and the single failure carried the two panics that mattered.
A run can pass its sentinel check while having panicked in a thread the gate
does not assert on.

```
==== DDR-961 N=20: 20 PASS / 0 FAIL | panics=0 ipc_timeouts=0 | kernel 26effe65 ====
```

| metric | result |
|---|---|
| gate verdict | **20/20 PASS** |
| `[BUG]` / `PANIC` / `#GP` / `#DF` | **0** across all 20 serial logs |
| `[recv]`/`[sub-approve]`/`[sub-alert] TIMEOUT` | **0** — no bounded wait expired in any run |
| `[trap]` lines per run | **exactly 2 in every run**, fully accounted for below |

Separately, a full-length `smoke-fs` serial capture on the same kernel shows all
IPC/bcast demo sentinels intact and unchanged: `[recv] blocking on endpoint`,
`[sub-approve] event type=` ×2 plus `done`, `[sub-alert] event type=` plus
`done`. The bounded waits changed no observable behaviour on the happy path.

### Every trap line accounted for — and two scoring bugs found on the way

The first scoring pass reported `unexpected_traps=11`. **It was wrong**, and how
it was wrong is worth recording because both defects produce plausible numbers
rather than obvious errors:

1. **`grep -c` already prints `0`** when nothing matches. The `|| echo 0`
   fallback appended a *second* zero, `$((panics+p))` then died on
   `syntax error in expression`, and the loop stopped after run 1 — while still
   printing a summary line reading `1/20 PASS`. A verdict was reported for a
   loop that never ran.
2. **Line-based exclusion of the deliberate boot faults fails under SMP.** The
   `[trap] user …` line is assembled from ~10 separate `kputs`/`kputdec` calls
   (`idt.c:355-363`). Each call is individually serialised — `irq_save()` in
   `console.c` is a *shadowing* local that takes `g_console_lock` (ADR-030
   stage 1), so `kputs` really is cross-CPU safe — but the lock is **released
   between calls**, so another CPU can interleave its own output mid-line:

   ```
   [trap] user [boot-load] SYSTEST.ELF t=#PF page fault pid=180
   ```

   That is the `WXVIOL.ELF` trap with a `[boot-load]` line spliced through it.
   The ELF name is displaced, so `grep -v WXVIOL` does not exclude it and it
   scores as "unexpected".

Corrected accounting over all 20 runs:

| | runs |
|---|---|
| total `[trap]` lines exactly 2 | **20 / 20** |
| exactly one `METRIC.ELF` trap | **20 / 20** |
| `WXVIOL.ELF` trap intact | 9 |
| `WXVIOL.ELF` trap interleaved with `[boot-load]` output | 11 |
| **unexplained traps** | **0** |

Both traps are the boot faults `WXVIOL.ELF` and `METRIC.ELF` take on purpose
every boot (W^X and read-only-page regressions). Nothing else faults.

### A finding this exposed, NOT fixed here
**A serial line assembled from multiple `kputs` calls can be split by another
CPU's output under `-smp 4`.** Per-call atomicity is guaranteed; per-line
atomicity is not. `kwrite` documents itself as emitting "n bytes as a single
locked unit" precisely because a multi-call line has no such guarantee.

Every gate in this tree asserts on serial patterns, so any gate matching a whole
line is exposed to this intermittently under SMP — which makes it a candidate
contributor to the intermittent-failure classes this project keeps chasing.
Recorded with evidence, deliberately not fixed: holding the console lock across
a whole logical line is a change to the console hot path and its lock ordering
(`klog_lock` nests inside, the console lock outside), and it deserves its own
DDR and its own verification rather than a tail-end edit.

## 6. Explicitly not done

- `ipc_ring_pop` / `ipc_ring_push` are lock-free and non-blocking (they return
  `1` for empty/full). Nothing to bound.
- `sched_block_on`'s `!current_thread` path unlocks and returns, breaking its
  own lock-held-on-exit contract; `sched_block_timeout` inherits it. Only
  reachable before the scheduler exists, so no caller can hit it. Recorded, not
  changed — it is not this DDR's defect.
