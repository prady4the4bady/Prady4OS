# DDR-1062 — OPEN-2: the first CI-side rate bound, and DDR-1009's 25% is refuted

**Status:** MEASURED. OPEN-2 remains **OPEN** — no mechanism named, no fix.
**Date:** 2026-09-05
**Relates to:** DDR-1009 (the 25% figure), DDR-1049 (the detector that makes a
green run mean something), DDR-1019/1006/1010 (the three producers), DDR-1023
(local route exhausted), DDR-1047/1060 (lock_stat), DDR-1055/1056 (the splice
class that accounts for the reds).

---

## 1. Why this measurement is possible now and was not before

DDR-1023 established that the LOCAL reproduction route is exhausted — 56 clean
runs across the two kernels that matter — and that **the live evidence is
CI-side**. But until DDR-1049 a CI green did not mean what it looked like: a
single CPU that panicked and died before its banner left **every channel empty**
(no `NEXUS KERNEL PANIC`, no `panics_silent=`, no `panic_stage=` because it was
gated behind the loser-only counter, and the capture deleted on a PASS), so
**a run could go green with a panicked CPU in it**.

DDR-1049 closed that hole: `g_panic_stage` is set by the **winner** the instant
it claims the latch, and `panic_stage=` is in `GLOBAL_FORBIDDEN`. So from
`32cb8ad` (2026-09-03) onward, a green suite genuinely asserts that **no CPU
claimed the panic latch and no `[apfreeze]` fired**.

That is what makes counting greens meaningful, and it is why the window starts
there rather than earlier.

**Detectors verified armed at the time of writing** (`GLOBAL_FORBIDDEN` reads
**76** entries, matching §NON-NEGOTIABLE 6's stated count, so the list is intact
rather than silently emptied): `apfreeze`, `panic_stage=`, `gs FAIL`,
`NEXUS KERNEL PANIC`.

## 2. The measurement

**42 `pradyos-ci` suite runs** on `dev/phase1-seyp3n` at or after `32cb8ad`,
across **19 distinct SHAs**, mixed `push` / `pull_request` / `workflow_dispatch`.

**Occurrences of `[apfreeze]`, `panic_stage=` or `gs FAIL`: ZERO.**

**All 42 count as observations, including the 4 reds.** A red suite still booted
its gates and still ran `boot_test.sh`'s global forbidden scan, so an
`[apfreeze]` would have named itself there too. Counting only the 38 greens
would understate the evidence.

### 2.1 Every red is attributed, and none is an AP freeze

This is the load-bearing check — a red left unread could BE the artefact:

| SHA | suite(s) | failing gate | attribution |
|---|---|---|---|
| `c8c93ed` | push + PR | `smoke-actiondel` | DDR-1056's splice class, recorded UNATTRIBUTED |
| `b7ff2a3` | push | `smoke-nethammer` | DDR-1055's console splice — **since fixed** |
| `1efbb49` | PR | `smoke-surfclose` | splice class, recorded UNATTRIBUTED |

`b7ff2a3`'s capture was read directly: heartbeats run clean to `t=23500` with
`ymask` climbing, `rqcpus=2`, `preempt` advancing and `pmmfree` flat — a
**healthy SMP kernel that timed out on a sentinel match**, which is DDR-1055's
defect exactly, not a frozen CPU. `1efbb49`'s tail carries the gate's own
`PRADYOS_WM_CLOSE` / `PRADYOS_SURFACE_GONE` lines and a plain Makefile
post-check failure, with no forbidden pattern named.

## 3. The bound, and what it refutes

0 occurrences in *n* observations ⇒ 95% upper bound `1 − 0.05^(1/n)`:

| quantity | value |
|---|---|
| observations | **42** |
| occurrences | **0** |
| **95% upper bound on the per-suite rate** | **6.9%** |
| P(0 in 42 \| p = 0.25 — DDR-1009's figure) | **5.7 × 10⁻⁶** |
| P(0 in 42 \| p = 0.10) | 0.012 |
| P(0 in 42 \| p = 0.05) | 0.116 |

**DDR-1009's 25% per-suite rate is refuted for the current kernel**, at
p ≈ 6 × 10⁻⁶. CLAUDE.md already says of the older "~1 in 4" that it *"was one
session's small sample and has not held up; stop quoting it as a rate."* This
supplies the replacement number rather than merely repeating the warning:
**below 6.9% at 95%, and 10% is itself unlikely (p = 0.012).**

Per-**suite** is the right unit because per-suite is what DDR-1009 measured, so
the two are directly comparable.

## 4. What this does NOT claim

- **OPEN-2 is not closed and no mechanism is named.** §NON-NEGOTIABLE 3 still
  forbids a fix. A rate below 6.9% is still a rate, and 42 observations cannot
  distinguish "fixed" from "rare".
- **These are 42 suite runs across 19 SHAs, not 42 independent binaries.** Many
  of those SHAs are docs-only and share a kernel. Pooling per-suite across SHAs
  that share a binary is DDR-1009's own method and it argued that pooling is the
  *better* bound — but it is not 42 binaries and must not be reported as such.
- **The detectors cover what they cover.** DDR-1049 records its own residual: a
  panic that faults between entering the panic path and *winning* the CAS is
  still invisible — a few instructions wide. A CPU that froze in a way neither
  `[apfreeze]` nor the panic latch can see would not appear here.
- **Nothing is claimed about `main`.** This window is `dev/phase1-seyp3n`.

## 5. The instrument set is now COMPLETE for discrimination

Each of DDR-1019's three producers now self-identifies, so the next occurrence
is diagnostic rather than ambiguous:

| producer | discriminator | status |
|---|---|---|
| panic-arbitration loser's `cli; hlt` (DDR-1019) | `panic_stage=` printed whenever the latch is claimed | DDR-1049 |
| AP wedged in its timer ISR (DDR-1006) | `[apfreeze]` + `rip=` + `bt=`, resolved per §INV.18 | shipped |
| broken SWAPGS at syscall entry (DDR-1010) | `gs FAIL (syscall ctx)`, continuous probe | shipped |
| **any of them — is it a lock wait?** | `lock_stat` `waiters=` on the `[apfreeze]` path | **DDR-1060** |

DDR-1060 is what completes it, and the **negative** reading is the valuable half:
a frozen CPU with **zero `waiters` anywhere** now means the freeze is *not* a
lock wait — an answer the instrument could not produce in either direction
before. Reachability was checked rather than assumed: the dump is ordered after
the `[apfreeze]` line, and **NMI is non-maskable, so it lands even on the
`cli; hlt` producer**.

## 6. DDR-1006 §7's "which loop iteration" — ASSESSED, NOT BUILT

§7 asked for *"more state at the freeze (which lock, which loop iteration)"*.
**"Which lock" is delivered** (§5). "Which loop iteration" is **not built, and
should not be**:

- It requires a per-iteration counter on hot loops that are live during the
  freeze — i.e. **always-on work on exactly the paths where the race lives**.
  That is the cost DDR-1047 refused, for the reason DDR-1010 recorded about its
  own probe: *"it adds work to the very syscall path where the race lives, so it
  may perturb what it measures."* An instrument that can **move** OPEN-2 is
  worse than no instrument.
- Opt-in is not the escape — DDR-1010's other rule, which DDR-1043 then found
  literally true of the QMP dump: an opt-in instrument is guaranteed OFF in CI,
  the only place OPEN-2 has ever appeared.
- And the marginal value is small now: `rip=` plus `bt=` already name the site,
  and `waiters=` answers what it was waiting for. The iteration index would
  refine a location that is already resolved.

## 7. What to do next

**Nothing, until an artefact appears.** The correct posture is the one DDR-1023
set and this DDR now quantifies: the local route is exhausted, the CI route is
armed and self-diagnosing, and the rate is bounded low enough that manufacturing
occurrences is not feasible either.

On the next `[apfreeze]`: resolve the RIP **against its own binary** (§INV.18 —
three producers are told apart only by RIP), then read `waiters=` to decide lock
wait vs not, and `panic_stage=` to decide panic vs genuine freeze.
