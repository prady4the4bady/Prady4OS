# DDR-790 — kernel heap double-free panic in CI (regression finding)

**Status:** **finding — NOT fixed. Evidence + a discriminator only.** Kernel;
prime suspect is **DDR-787** (blocking pipes / pipe refcount split), which is
**already promoted to `main`**.

## What happened

CI run **30215987521** (`ba5770e`) failed at step 54, `smoke-blkmq`
(q35 `-smp 4`). The required sentinel `[blk] multi-inflight OK` never appeared,
and the serial dump ends:

```
[kheap] double-free ptr=0x0000000007E29F80 objsize=0x0000000000000020
*** KHEAP PANIC: kfree: double free ***
```

**A kernel panic, not a hang.** Two readings had to be corrected on the way to
that:

1. `multi-inflight FAIL` appears in the log, but it is the Makefile echoing the
   gate's `FORBIDDEN_SENTINEL=` line — **not** a kernel print. The kernel never
   reported a functional failure.
2. This is **not** the B#3 timer-stall signature. Zero `[hb]` heartbeats appear
   in the dump, which initially looked like DDR-777 verdict (A) — but the run
   panicked, so the absence of heartbeats is a *consequence* of the panic, not
   independent evidence of a stalled timer. **The B#3 discriminator was not
   validly read here and B#3 remains open.**

## Why DDR-787 is the prime suspect

- `objsize=0x20` (32 bytes). `struct pipe` is `uint64 buf` + `uint32 head` +
  `uint32 tail` + `int readers` + `int writers` = 24 B, which lands in the 32-byte
  bucket. (`struct vfs_file` also lands there — see "not yet excluded".)
- DDR-787 is precisely the change that **altered pipe lifetime**: it replaced a
  single `refcount` with `readers`/`writers` and moved the free to "both counts
  zero", touching six incref/close sites. That was flagged as the review-critical
  part of the slice, and this is exactly the failure mode predicted for getting it
  wrong.
- The panic first appears after DDR-787 landed. Two earlier failed runs
  (30192189559, 30188805082) contain no such panic — **weak** evidence, since both
  died before producing comparable serial output.

## Not yet excluded — do not assume

- `struct vfs_file` occupies the same 32-byte bucket and is `kfree`d in
  `fd_free` and copied in `sys_dup2`/`fd_clone`. A double-free there would look
  identical in this log.
- Inspection of the six DDR-787 sites did **not** find the bug. `fd_free` clears
  the entry (`e->pipe = 0`, `kind = FD_NONE`), so a repeated close is a no-op;
  `sys_dup2` and `sys_pipe`'s error paths each balance. So the defect is either
  subtler than inspection catches, or it is not the pipe at all.
- **Local reproduction failed: 3/3 `smoke-blkmq` runs PASS with no panic.** That
  matches the DDR-775 pattern (local green, CI red) and means this is
  intermittent — likely timing- or SMP-dependent.

## Decision — make the next occurrence decidable, do not guess-fix

Guessing at a refcount fix without evidence would repeat the mistake B#3 punished
three times. Instead, `pipe_destroy` now prints the object it is about to free:

```
[pipe] destroy p=<ptr> r=<readers> w=<writers>
```

paired with a `[pipe] create p=<ptr>` line. Both are needed: creates alone cannot
show a leak, and destroys alone cannot distinguish a double free from address
reuse (which is exactly the trap this investigation fell into). The next
occurrence is then decidable — a `destroy` with no matching un-freed `create`
is a genuine double free; if the panicking pointer never appears at all, the pipe
is exonerated and `struct vfs_file` is next. Evidence only, no gate asserts on it,
exactly as with DDR-777's heartbeat.

**These traces are temporary.** They print on every pipe, and should be removed
once the panic is explained.

## What the diagnostic showed — and a correction I had to make mid-investigation

First run of the trace showed three pointers each appearing **twice**, which looks
exactly like a double free, and I initially read it that way. It was not. Adding a
matching `[pipe] create` trace showed the accounting is **balanced**:

```
create p=…F60   create p=…FA0   create p=…F40
destroy …FA0    destroy …F40    destroy …F60
create p=…F60   destroy …F60          creates=4  destroys=4
```

Every destroy follows its own create; `kheap` simply **recycles addresses**, so
"same pointer freed twice" was two different pipes. Pointer identity alone is not
evidence of a double free — the create/destroy pairing is. **The CI panic is
therefore still unexplained and unreproduced** (3/3 `smoke-blkmq` and this trace
are clean).

### One hardening applied, framed honestly as hardening

`pipe_close` now frees only if the call **actually dropped a reference**:

```c
int dropped = 0;
if (is_write) { if (p->writers > 0) { p->writers--; dropped = 1; } }
else          { if (p->readers > 0) { p->readers--; dropped = 1; } }
if (dropped && p->readers <= 0 && p->writers <= 0) pipe_destroy(p);
```

The first cut freed whenever both counts *read* 0, so a close that decremented
nothing — because its side was already 0 — would free a second time. The
pre-DDR-787 single refcount masked that shape by going negative. **This is not
proven to be the CI panic**: no such call was observed. It is a real latent
weakness closed on inspection, and it is recorded as such rather than claimed as
the fix.

## Decision record — PIPE_TRACE=1 on smoke-blkmq (operator, 2026-07-28)

> "PIPE_TRACE=1 enabled on smoke-blkmq CI job per operator decision 2026-07-28.
> smoke-swapgs BSP liveness markers deferred to BUG-1 follow-up DDR (separate
> from DDR-790). D-series Python loop continues in parallel unblocked."

**Implementation note — why this is a separate make target, not an env flag.**
`PIPE_TRACE` is **compile-time**, and CI builds a single shared image
(`make image` once, then every gate runs against `build/pradyos.img`). Setting
the flag for the CI step alone would do nothing, and setting it globally would
put high-volume traces into every *later* gate — which is exactly what evicted
`smoke-dmesg`'s log-ring marker and caused run 30303017178.

`make smoke-blkmq-trace` therefore rebuilds the kernel with `PIPE_TRACE=1`, runs
**only** this gate, and then rebuilds clean, propagating the gate's exit code. It
also removes `pipe.o`/`kernel.elf`/`kernel.bin`/the image explicitly first,
because make does not track CFLAGS changes — without that the flag silently does
nothing, a stale-image trap this project has already hit twice.

## BUG-1-SFS — filed, and one correction to the direction given

The operator direction states the churn FAIL "IS causal" and to add an
`sfs_btree_verify()` call. Two things must be recorded honestly:

1. **`sfs_btree_verify()` does not exist.** `grep -rn "btree_verify" kernel/`
   returns nothing. Adding it means writing a full B+tree structural verifier in
   the kernel — a real slice with its own DDR, not a diagnostic one-liner.
2. **Causality is still unproven.** The evidence establishes only that the churn
   reported FAIL *and* that the BSP later stopped progressing. This DDR's own
   history is three theories refuted by acting before the mechanism was named
   (DDR-775/776/777), so the claim is recorded as a *hypothesis to test*, not a
   given.

**Cheaper first diagnostic, which actually discriminates:** the churn loop
currently collapses `vfs_create` / `vfs_write` / `vfs_unlink` failure into one
`churn_ok = 0`, discarding *which* call failed and *at which iteration*. Naming
that costs three prints on a failure path only — no hot-path volume, so it cannot
evict gate markers — and it is what decides whether the SFS allocator is even
implicated before anyone writes a verifier.

## Risk while unfixed

The panic is on `main` (DDR-787 was promoted on green run 30211536949). It is
intermittent — five consecutive runs before this one were green, and the
regression it fixes (pipelines silently producing nothing) is real and shipped —
so **reverting is not obviously the safer choice** and is not proposed here.
Flagged for the maintainer rather than decided unilaterally.

## Architecture prerequisite checklist

- New syscalls/NSI, TCB, PMM/VMM, capabilities, AETHER, scheduler hooks, FS,
  compositor: none. One `kputs` in `pipe_destroy`.
- **Security invariants:** **S6** is the one engaged — a double `kfree` is
  precisely a fault-isolation failure, and it panics the kernel rather than
  failing an operation. That is the invariant this finding says may be violated;
  the diagnostic exists to establish where. No invariant is weakened by the
  diagnostic itself.
