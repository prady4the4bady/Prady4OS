# DDR-980 — Remove the per-CPU heartbeat instrument; record OPEN-13 (kheap double-free)

Status: ACCEPTED. Number verified free in **both** `docs/ddr/` and
`docs/decisions/` (§INV.4).

Two things, related only by the CI runs that prompted them.

---

## 1. The `cputicks[…]` heartbeat instrument is removed

DDR-977 §7.2 added every CPU's tick counter to the `[hb]` line to remove a
confound (the block-timeout path can only report on the CPU it points at). It
worked: it produced the CPU-3 freeze evidence *and* the clean negative control
that DDR-977 §7.3 turns on.

**It is being removed anyway, because keeping it is a bad trade.**

### What it cost

It ran inside the timer ISR: four `percpu_get()` reads plus four `kputdec()`
calls, emitting ~30 extra characters to a 16550 UART, once per 500 ticks.
`kputs`/`kputdec` on that path are not free — the UART is slow and the write
happens with the ISR on the stack.

**DDR-947 recorded this exact hazard in this codebase:** an instrument added to
the timer ISR "coincided with the failure rate going 2/12 -> 9/14, i.e. the
instrument was heavy enough to move the timing it was measuring". That is why
`cur=` on the same line is gated behind `rd > 8`. I judged fixed-width numerics
safe by comparison. That judgement is not obviously wrong, but it is not
obviously right either, and the evidence below is not reassuring.

### The evidence, stated at its real strength

Red **suites** per push on this branch:

| window | pushes | pushes with a red suite |
|---|---|---|
| before the instrument (`ea4601e`…`fa854d3`) | ~10 | **1** (`83554f9`, `smoke-wmmax` — re-run green) |
| after it (`b43d6b0`, `ff56d47`, `848861b`) | 3 | **3** |

**This is suggestive, not proof, and it must not be written up as proof.** The
three reds are three *different* gates (`smoke-blk-integrity` ring-0 panic,
`smoke-wmmax`, `smoke-blkmq-trace` kheap double-free), and one of them —
`smoke-wmmax` — was already failing *before* the instrument existed. n=3 against
n=10. A fair reading is "the rate looks worse and the instrument is a known
mechanism for making it worse", not "the instrument caused these".

### Why removal is right regardless

The decision does not rest on the correlation. It rests on the trade:

- **The diagnostic value is already spent.** DDR-976/977 have the measurement.
  The instrument is not needed to hold that conclusion.
- **The risk is documented and specific** (DDR-947), and it is on the default
  build path, affecting all 149 gates.
- **The cost of being wrong is asymmetric.** If it does perturb timing, it is
  corrupting every gate and blocking the 3-consecutive-greens the release needs.
  If it does not, removing it loses nothing already banked.

Re-add it behind an opt-in build flag (as `BSP_LIVENESS` and `PIPE_TRACE` are)
if the CPU-3 work resumes. Do not put it back on the default path.

**Kept:** the DDR-977 `[vblk] compl wait timeout unit= dest_cpu= … ticks[…]`
instrument. It executes only *after* a 5-second deadline has already expired, so
it cannot perturb the timing of a healthy run — the property that made it safe
in the first place.

---

## 2. OPEN-13 — kernel heap double-free (new, good artefact)

`smoke-blkmq-trace`, shard 4, head `848861b` (a **docs-only** commit — one `.md`
file, so not a regression from it):

```text
[boot-load] TIME.ELF t=247
[kheap] double-free ptr=0x0000000007FD0BA0 objsize=0x0000000000000080

*** KHEAP PANIC: kfree: double free ***
```

This is **not** OPEN-2's SMP flake, despite `smoke-blkmq-trace` being on that
list and being a `QEMU_SMP=4` gate. Different signature entirely; treating it as
OPEN-2 would be colour-matching.

**It is the detector working as designed.** `kheap.c:139` says it exists so "a
rare SMP double-free is diagnosable from the serial log (size class -> which
structure)". It has now fired, and `KHEAP_DEBUG` is unconditionally `1`, so this
check is live in the shipped kernel too — not a debug-build artefact.

**What the size class says, and what it does not.** `objsize=0x80` is 128, which
is a **generic `kmalloc` size class** (`size_classes[] = {16,32,64,128,…}`), not
one of the dedicated caches (`pcb`=512, `cap`=16, `ipc`=256). So the detector's
"size class -> which structure" mapping does **not** resolve here: any
`kmalloc(n)` with 65 ≤ n ≤ 128 lands in it. The structure is not identified, and
guessing one from the size alone would be exactly the error this project keeps
retracting.

**Caveat on the build:** the gate rebuilds with `PIPE_TRACE=1` (recompiling
`pipe.c` only) despite its name. Whether the double-free needs that build is
unknown from one capture.

**To narrow it** the detector needs the *caller*, not just the size: record the
allocating and freeing return addresses per object under `KHEAP_DEBUG` and print
both on the panic. That is a real change to a hot allocator path and should be
opt-in — and it should not land while the branch is trying to accumulate greens.

Tracked as **OPEN-13** in `CLAUDE.md`. Rate: 1 occurrence.
