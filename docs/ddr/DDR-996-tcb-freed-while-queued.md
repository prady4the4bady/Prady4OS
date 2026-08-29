# DDR-996 — a TCB is freed while still linked on a runqueue

**Status:** IMPLEMENTED, GATED, MUTATION-CHECKED.
**Artefact:** CI run 32702096039, shard 2, `smoke-blk-integrity`, commit `f74e5c5`.
**Bears on:** OPEN-12 (`component: NEXUS isr` ring-0 `#GP`) — see §6 for what is
and is not claimed about that.

---

## 1. The artefact

```
*** NEXUS KERNEL PANIC ***
component: NEXUS isr
exception: #GP general protection  vector=0x0D  error=0x0
RIP=0xFFFFFFFF80012D6A          CS=0x08          RFLAGS=0x82
RAX=0xDEADBEEFDEADBEEF
RCX=0xFFFFFFFF80113CA8  RDI=0xFFFFFFFF80113CA8
```

preceded immediately by init respawning a failing service:

```
[svc] exit missing pid=52 st=127
[svc] restart missing 2/3
[svc] start missing pid=57
```

**This is the first READABLE capture of this panic.** DDR-979 merged make's
stderr into the job log (`2>&1`) precisely so the next occurrence would carry an
intact `exception:`/`RIP=` block instead of the overwritten one it had to give
up on. That change did its job — everything below is read off the register dump,
not inferred.

## 2. Reading it

`RIP` resolves to **`fair_candidate + 0x3A`**, which disassembles to:

```
d4f:  mov  -0x8(%rbp),%rax      ; rax = q
d53:  mov  0x8(%rax),%rax       ; rax = q->head
d57:  mov  %rax,-0x28(%rbp)     ; c = q->head
d5b:  cmpq $0x0,-0x28(%rbp)     ; if (!c) done
d66:  mov  -0x28(%rbp),%rax
d6a:  cmpl $0x0,0x14(%rax)      ; <-- FAULTS: c->state, rax = POISON
```

So the faulting dereference is the **first** loop iteration: `q->head` itself
held `0xDEADBEEFDEADBEEF`. That constant is `PMM_POISON` (`kernel/mm/pmm.c:23`) —
what the physical allocator stamps over a page it has just freed.

`RDI` = `0xFFFFFFFF80113CA8` is a kernel `.bss` address, i.e. `q` points at a
**static** `g_rq[]` entry (`sched.c:94`). The runqueue structure was not freed;
its `head` pointer was made to point at a freed page.

Non-canonical, so `#GP` and not `#PF` — consistent with the vector, and the
reason this class of corruption prints `#GP` while DDR-985's printed `#PF`.

## 3. The mechanism

A thread carries **two** independent links:

| list | field | purpose |
|---|---|---|
| all-threads ring | `t->next` | every live thread, for walks (`wait4`, `ps`, reaper) |
| per-CPU ready FIFO | `t->rq_next`, `q->head/tail`, flag `t->rq_on` | pickable threads |

`sched_exit()` (`sched.c:1578`) sets `state = THREAD_ZOMBIE` and calls
`schedule()`. **It does not unlink from the runqueue** — the thread stays linked
with `rq_on == 1`. That is survivable on its own, because `fair_candidate` skips
non-READY entries and `rq_take` drops them lazily.

Both reap paths then free the TCB:

- `reaper_thread` (`:1636`) → `sched_ring_unlink(victim)` → `sched_free_tcb`
- `sched_destroy` (`:1141`, the `wait4` path) → `sched_ring_unlink(t)` → `sched_free_tcb`

and `sched_ring_unlink` (`:1079`) walks **only** `->next`. It never touches
`rq_next`, `q->head`, or `rq_on`. `sched_free_tcb` calls `switch_wait_offcpu(t)`
— which waits for the victim to be off **CPU** — and then `kfree(t)`. Nothing
anywhere waits for it to be off the **runqueue**.

`rq_unlink()` has exactly one call site in the file: the pick path (`:416`).
Nothing on the exit or reap path calls it.

So:

1. thread exits, still linked on `g_rq[c]`, `rq_on == 1`
2. it is reaped and `kfree`d before any `rq_take` pass happens to pop it
3. the page is stamped with `PMM_POISON`
4. the next walker reads the dead TCB's `rq_next` — now poison — and either
   dereferences it (`fair_candidate`, this capture) or *writes it into
   `q->head`* (`rq_unlink :281`, `rq_take :390`), which corrupts the queue
   permanently for every later pass.

Step 4's second branch matters: the corruption is not confined to one bad read.
Once poison reaches `q->head`, that CPU's runqueue is unusable.

### Why it is rare, and why THIS gate hit it

The window is "exits while queued, then reaped before the queue is next
drained". Ordinary threads are popped by `rq_take` long before the reaper looks
at them. `[svc] restart missing 1/3 … 2/3` is init respawning a service that
exits immediately (`st=127`) — a spawn/exit storm that opens the window
repeatedly, and `wait4` (init is alive, so `sched_destroy`, not the reaper)
frees each corpse promptly.

## 4. The fix

Unlink from the runqueue before freeing, in `sched_free_tcb`, after
`switch_wait_offcpu`. A TCB reaches `sched_free_tcb` already unlinked from the
all-threads ring, so **no walker can discover it to re-push it** — the runqueue
is the only remaining reference, and removing it there is final.

`rq_on` is a bare flag: the TCB does not record *which* CPU's queue it is on. Two
options were considered:

- **add `t->rq_cpu`** — cheaper unlink, but a new `struct tcb` field, and
  §NON-NEGOTIABLE 10 (`kmalloc` does not zero) makes every new field an
  initialiser bug waiting to happen in `sched_create`. Rejected: the cost is
  paid on every thread creation to save work on a path that runs at reap time.
- **scan all `g_rq[]` under each queue's lock** — O(PERCPU_MAX × queue length)
  at reap only. Chosen. `rq_unlink()` already does the per-queue work and
  already clears both `rq_next` and `rq_on`, so this is a loop around an
  existing, tested primitive rather than new list surgery.

The scan is guarded by `rq_on`, so the common case (already popped) costs one
atomic load.

## 5. Gate — and why the obvious one would be vacuous

The trap: add the unlink, then assert "no panic". A boot without the defect also
does not panic, and the window is rare, so such a gate is green either way — the
DDR-988 §9 failure mode.

The gate must instead prove the **dangerous state actually arises**. So the fix
counts what it catches, and the probe forces the window:

- **arm A** — a spawn/exit storm (short-lived threads exiting immediately,
  reaped promptly, mirroring `[svc] restart`) must drive
  `[rqfree] caught=N` with **N > 0**. That is the assertion that a TCB really
  does reach `sched_free_tcb` still queued. If N is 0 the probe is not
  reproducing the condition and the gate says so rather than passing quietly.
- **arm B** — the boot completes with no `#GP` and no poison in any queue head.

**Mutation (required):** keep the counter, remove the `rq_unlink` call. Arm A
still reports N > 0 (the state arises) and the boot must then corrupt — panic or
a poisoned head. A mutant that stays clean means the storm is not actually
hitting the window and the gate is decoration.

`[rqfree]` goes in `GLOBAL_FORBIDDEN` **only after** the fix is in and the count
is expected to be zero in steady state — see §7. Until then it is a measurement,
not a sentinel.

## 6. What is NOT claimed

- **This is not proof that OPEN-12 is closed.** OPEN-12's original capture
  (`b43d6b0`, shard 0) had its `exception:`/`RIP=` block destroyed by the very
  interleaving DDR-979 later fixed, so its faulting address is *unknown*. It was
  `#GP`, as this is, and `component: NEXUS isr`, as this is — but CLAUDE.md's
  OPEN-13 row already warns that matching on `component:` alone is colour-
  matching, since every non-recoverable ring-0 vector prints it. The honest
  statement is: this is a real UAF that produces exactly OPEN-12's *observable*,
  and it is the strongest candidate yet. OPEN-12 closes when a fixed kernel runs
  the campaign without recurrence, not on this DDR.
- **Not claimed to be OPEN-1.** OPEN-1's CI route is a hang with no panic
  (DDR-990 §12); this one panics loudly.
- **Not claimed to be OPEN-13.** That is a `kheap` double-free at
  `objsize=0x80`; `struct tcb` comes from the 512-byte cache.
- **Not caused by the PR that surfaced it.** `f74e5c5`'s scheduler diff is +36
  lines / 0 deletions, entirely the additive DDR-994 `yield_stall_note`, and
  touches no enqueue, dequeue, reap or free path. DDR-994's per-spin counter can
  shift timing, which is a plausible reason the window was hit now rather than
  earlier — but timing perturbation exposes a defect, it does not create one.

## 7. Open question deliberately left open

Whether the *right* long-term fix is unlink-at-free (this DDR) or
unlink-at-exit (`sched_exit` removing itself from the queue as it becomes a
zombie). Unlink-at-exit is tidier and shortens the window to nothing, but it
runs on the exit path with the topology lock held and the thread still on its
own kernel stack, which is the exact region DDR-SMP-exit-stack-race and rq-2 D4
were written about. Unlink-at-free is chosen here because it is provably
sufficient (§4: no discoverable reference remains) and touches no live-thread
path. Revisit post-1.0 with a measurement, not a preference.


---

## 8. Results (2026-08-24)

| kernel | `made` | `caught` | `leaked` | gate |
|---|---|---|---|---|
| fixed `1d27b148fa8b26b7` | 16 | **16** | **0** | PASS |
| mutant `395f68e5918c040e` (counter kept, unlink removed) | 16 | 16 | **16** | **FAIL** |

`caught=16` out of 16 is the load-bearing number: **every** victim reached
`sched_free_tcb` still linked on a runqueue. The window is not exotic — with a
create/unblock/destroy sequence it is the norm, which is why a spawn/exit storm
finds it.

### 8.1 Two things in §4–§5 were wrong, and the measurement caught both

Recording these because both were mistakes of the type this repo keeps
re-learning, not incidental slips.

**(a) The first arm B was decoration, and the mutant proved it.** §5 specified
arm B as "walk the queues, flag a non-canonical link". Run against the mutant it
reported `caught=16` and a *clean walk* — the fix removed, the gate still green.
The reason: `kfree()` returns a TCB to its slab cache, so the dangling pointer
stays perfectly canonical. `PMM_POISON` only appears once that page is recycled
to the physical allocator, many frees later. **The poison in the artefact was the
eventual symptom; the invariant is "no runqueue references a freed TCB".** Arm B
v2 tests that directly — `rq_references(v)` immediately after each destroy,
before any allocation can reuse the address and mask it — and the mutant then
fails 16/16. Had the precommitment in §5 ("a mutant that stays clean means the
gate is decoration") not been written down first, the weak version would have
shipped green and proved nothing.

**(b) A first arm-B draft rejected a perfectly good pointer.** It tested
`ptr >= 0xFFFFFFFF80000000` and immediately flagged a live TCB at `0x07C64000`.
This kernel's heap and kernel stacks are identity-mapped LOW — which the artefact
itself states, `RSP=0x0000000007DABDD8` in the panic. A high-half assumption
imported from other kernels, contradicted by the very dump being diagnosed.

### 8.2 The mutant did not build, and the hash check is the only reason I know

The first mutation run reported `caught=16 / leaked=0` — mutant apparently
survives. It had not been built: removing the unlink leaves `prev` unused, which
is `-Werror` under §NON-NEGOTIABLE 19, so `make` failed and the *previous*
kernel binary was still on disk and still ran. The kernel hash was byte-identical
to the fixed one, which is the only thing that exposed it. This is DDR-990 §8's
precaution earning its place a second time: **a mutation result without a
distinct kernel hash is not a result.** (§INV.10 is the same lesson in a
different costume.)

### 8.3 The probe's first draft hung the boot for ~22 s

`sched_rqfree_probe` originally held `irq_save()` across the unblock/destroy pair
to stop this CPU rescheduling into the victim. Boot reached `t=2468` (normal
~210) and stalled: `sched_free_tcb -> switch_wait_offcpu` (`sched.c:479`) is an
**unbounded** spin, and spinning with `IF` clear is precisely the DDR-981
livelock — committed nine hours earlier, in this same session. The mask was not
needed: unblock and destroy are adjacent, and `caught=16/16` shows the window is
hit without it.

Worth noting for DDR-994's ledger: `switch_wait_offcpu` at `sched.c:479` is a
**fifth** unbounded yield-free spin, and it is not one of the four DDR-994
instrumented. It is not on a ring-3 path, so it is out of OPEN-1 route 1's scope,
but it belongs in the count.

### 8.4 Regression

`smoke-shell` 5/5, `smoke-blkmq`, `smoke-blk-integrity`, `smoke-rqstress-liveness`,
`smoke-smp`, `smoke-resched`, `smoke-yieldstall` all PASS on `1d27b148`.
`ci-shard-check` OK (154 gates), `ci-probe-rodata-check` OK.
`smoke-blk-integrity` is the gate whose CI failure produced the artefact.
