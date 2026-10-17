# DDR-1049 — A lone silent panic left a GREEN run, and the field built to name it could not print

**Status:** FIXED + detector added + M1 mutation-proven.
**Bears on:** OPEN-2, and on how much "CI was green" is worth.

---

## 1. The defect, in one sentence

`panic_stage` — the field DDR-1019 added specifically to say *"the winner claimed
the panic latch and never reached the banner"* — was printed only when
`g_panic_extra != 0`, and `g_panic_extra` is incremented **only in the loser
branch of the panic CAS**. So the field could not print in the one case it was
built for.

## 2. Why that is worse than a missing print

Follow a single CPU that panics and dies before its banner reaches the UART
(DDR-1019 §2 observed exactly this shape in CI 33627355396 / `81274f4`):

| evidence channel | what it shows |
|---|---|
| `*** NEXUS KERNEL PANIC ***` | **absent** — the winner never got that far, so `GLOBAL_FORBIDDEN` never trips |
| `[apfreeze]` | only if some CPU also froze long enough to be sampled; not guaranteed |
| `panics_silent=` | **not printed** — `g_panic_extra` is 0, because nothing *lost* the CAS |
| `panic_stage=` | **not printed** — gated behind the same `g_panic_extra` |
| the serial capture | **deleted**: `boot_test.sh` `serial_rm`s it on the PASS path, and CI never sets `KEEP_SERIAL` |

Every channel is empty, the gate's own sentinels can still all be present, and
**the run goes green with a panicked CPU in it.**

That is not a hypothetical reading of the code. DDR-1019 established the
"winner printed nothing" mechanism from a real CI capture; what this DDR adds is
that in the *single*-panic case the instrument built to catch it is inert.

**This bears directly on the release decision.** DDR-1009 already recorded that
this kernel satisfied the 3-green rule twice at a measured 25% per-suite failure
rate, i.e. that green is a weaker signal here than it looks. A green run that can
contain an unreported panic makes it weaker still.

## 3. The fix

One predicate:

```c
{ extern uint64_t g_panic_extra; extern uint64_t g_panic_stage;
  if (g_panic_extra || g_panic_stage) { ... } }
```

`g_panic_stage` is set by the **winner** the instant it claims the latch
(`idt.c:845`, `= 1`), before anything is printed. Gating on it means *any*
claimed panic surfaces in the heartbeat; `panics_silent=` keeps its old meaning
(a second panic occurred) and is still printed, now as a value rather than as the
gate.

`g_panic_extra` is retained in the condition rather than dropped: the two are
independent, and a loser-only path (should one ever exist) should not become
invisible in the course of fixing the winner path.

## 4. The detector

`panic_stage=` is added to `GLOBAL_FORBIDDEN`, so a claimed panic fails its gate
whether or not the winner lived to print the banner. This is the precedent DDR-981
set by adding `[apfreeze]`: the panic path's *behaviour* is unchanged; what changes
is that a silent one now names itself instead of passing.

Coverage, stated precisely:

- **stage 1** (claimed, nothing printed) — previously invisible, now caught **by
  this pattern alone**.
- **stage 2/3** (banner out) — already caught by `NEXUS KERNEL PANIC`; now caught
  twice, which is harmless.

### 4.1 §NON-NEGOTIABLE 6 — the terminator

That rule warns that the documented verification command's `sed` range ends at the
list's **last** entry, so appending breaks the very check that detects breakage.
`panic_stage=` was therefore inserted **before** the final line, leaving
`'[percpu] gs FAIL' '[percpu] current FAIL')"` as the terminator and the `sed`
command untouched. Measured, running CLAUDE.md's command verbatim:

```
before: 73
after : 74      (and NOT 0, which is the catastrophe that rule exists to catch)
```

CLAUDE.md's stated count is updated from `~73` to `74` in this same commit, as
that rule requires.

## 5. Measurement

### 5.1 Baseline — the detector must not redden healthy boots

`panic_stage=` is now a forbidden pattern for all 173 gates, so the load-bearing
question is whether a healthy boot ever emits it. It cannot: both globals are BSS
(zero) and `g_panic_stage` is written only on the panic path. Confirmed by
measurement rather than left to the argument — see §5.3.

### 5.2 M1 — the detector is live

A healthy boot cannot produce the string, so the arm is proven by **forcing**
`g_panic_stage = 1` without a panic, exactly as DDR-1030 forced its `idle2=`
branch and DDR-1047 forced its lock dump. The gate must go RED.

### 5.3 Results

**Baseline, PRE-fix kernel `1bdd581fc269516b`** — 3 healthy boots, `-smp 4`,
150 s each:

| run | heartbeats | `panics_silent` lines | `panic_stage` lines | `apfreeze` |
|---|---|---|---|---|
| 1 | 29 | 0 | 0 | 0 |
| 2 | 29 | 0 | 0 | 0 |
| 3 | 29 | 0 | 0 | 0 |

`grep -oiE 'panic[a-z_]*'` over a full capture returns **nothing at all** — a
healthy boot emits no panic-related string of any kind, so making `panic_stage=`
a forbidden pattern cannot redden a healthy gate.

**Control and M1, on the FIXED tree:**

| | kernel | result |
|---|---|---|
| control (healthy boot) | `091542611c3e4545` | **rc=0**, 29 heartbeats, **0** `panic_stage` lines |
| M1 (`g_panic_stage = 1` forced from the heartbeat, no panic anywhere) | `1efd516bac35f22b` | **rc=1**, caught by the GLOBAL scan (`DDR-791: forbidden in every gate, not only the one that owns it`) |

M1's capture carries the line the old predicate could not produce:

```
panics_silent=0 panic_stage=1
```

`panics_silent=0` (nothing lost the CAS, i.e. exactly one panic) together with
`panic_stage=1` (the winner claimed the latch and printed nothing) **is the
signature this DDR exists for**, and under the old `if (g_panic_extra)` gate that
line would not have been emitted at all.

Reverting M1 returns the kernel to `091542611c3e4545` **bit-for-bit**.

Regression on the shipped kernel: `hygiene_check.sh` **ALL SIX**, and the gate
suite in §7.1.

## 6. What this does NOT do

- **It does not fix any panic.** No cause is named and none is claimed. This makes
  an existing failure *reportable*; §NON-NEGOTIABLE 3 forbids more.
- **It does not close OPEN-2.** It removes one way OPEN-2 could have been hiding
  in green runs. Whether it *was* is unknown — every past green run is already
  gone, and this cannot be applied retrospectively.
- **It does not recover the serial capture on a passing gate.** A run that trips
  the new pattern now *fails*, and `serial_keep_fail` keeps the capture on the
  failure path — so the evidence survives from the next occurrence onward, not
  before.
- **A panic that dies before `g_panic_stage = 1` is still invisible** — i.e. one
  that faults between entering the panic path and winning the CAS. That window is
  a handful of instructions and is not covered.

## 7. Gate suite

Run on the shipped kernel `091542611c3e4545`, with the hash recorded before and
after so the gates provably ran the binary being shipped (DDR-1035's assertion,
applied locally). `smoke-selftest` is included deliberately: its case 5 is the
meta-test that exists to catch a broken `GLOBAL_FORBIDDEN` list (DDR-791), which
is precisely what this change touches.

```
kernel=091542611c3e4545
smoke-shell              rc=0
smoke-blkmq              rc=0
smoke-rqstress-liveness  rc=0
smoke-blk-integrity      rc=0
smoke-smp                rc=0
smoke-smppreempt         rc=0
smoke-selftest           rc=0      <- the GLOBAL_FORBIDDEN meta-test
kernel_after=091542611c3e4545
```

`smoke-selftest` passing is the load-bearing one: it is the gate DDR-791 built to
catch a `GLOBAL_FORBIDDEN` list that has been silently broken, and this change
edits that list.

## 8. Files

| file | change |
|---|---|
| `kernel/idt.c` | heartbeat panic block gated on `g_panic_extra \|\| g_panic_stage` |
| `tools/qemu_runner/boot_test.sh` | `panic_stage=` added to `GLOBAL_FORBIDDEN`, before the terminator line |
| `CLAUDE.md` | §NON-NEGOTIABLE 6 count `~73` → `74` |
| `docs/ddr/DDR-1049-silent-panic-detector.md` | this document |
