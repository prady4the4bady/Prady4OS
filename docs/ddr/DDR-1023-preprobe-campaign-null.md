# DDR-1023 — the DDR-1010 §9.3 pre-probe campaign: 0/20, and what that closes

**Status:** CAMPAIGN RUN, **null result, and the null is the finding.** It
settles DDR-1010 §9.2's open caveat in the direction §9.2 did *not* predict, and
it redirects OPEN-2 effort away from local reproduction. No code change. Also
records a methodology defect in my own campaign that would have made one of its
claims vacuous.

---

## 1. The question this was run to answer

DDR-1010 §9.2 left an explicit caveat. Its continuous SWAPGS probe sits **on the
syscall entry path where the race lives**, so its own 36/36 clean campaign might
be the probe perturbing what it measures rather than evidence about the defect.
§9.3 named the experiment that would tell the difference: campaign the
**pre-probe** kernel — the binary the ~1-in-4 failure was actually observed on.

## 2. Method, with thresholds fixed BEFORE the run

- **Kernel `29c792a8b8f3b056`**, rebuilt bit-for-bit in a worktree at `d7d2794`
  (the commit before the probe landed at `f9bdfeb`) and hash-verified.
- **Gate `smoke-blk-integrity`**, the gate DDR-1010 reproduced on.
- **N = 20**, serialized (one QEMU at a time, §NON-NEGOTIABLE 12).
- Ledger records the kernel hash **before and after every run** — the dual-hash
  column that caught two accidental mid-campaign rebuilds in an earlier session.
- **Decision rule, written down before starting:**
  - **≥1 failure** → the defect reproduces *without* the probe; §9.2's
    perturbation caveat is confirmed and there is a reproducible target.
  - **0/20** → bounds the pre-probe local rate **below 14% at 95%**
    (`0.86²⁰ = 0.049`), and the perturbation hypothesis is **not** supported.

If the rate really were the originally-observed 25%, `P(0 in 20) = 0.0032`.

## 3. Result

**20/20 pass. Zero failures. One kernel hash on all 40 recorded values, zero
drift.**

| | |
|---|---|
| N | 20 |
| failures | **0** |
| hash before/after, every run | `29c792a8b8f3b056` |
| runs discarded for hash drift | 0 |

**What licenses "no `[apfreeze]`":** `smoke-blk-integrity` runs through
`boot_test.sh`, and `[apfreeze]`, `[percpu] gs FAIL` and `NEXUS KERNEL PANIC` are
all `GLOBAL_FORBIDDEN` entries — a hit kills the run and fails the gate. **All 20
runs returned rc=0**, so none of the three appeared. That is the valid argument,
and it is *not* the one I first reached for (§5).

## 4. What this settles, and what it does not

**Settled: DDR-1010 §9.2's perturbation hypothesis is NOT supported.** The probe
is not what made that campaign clean — the kernel *without* the probe is clean
too, at 20 runs. The two campaigns bound their own binaries and **must not be
pooled** (DDR-1009 §8.3 permits pooling only across an identical binary):

| kernel | campaign | bound at 95% |
|---|---|---|
| `9623c163cd479043` (post-probe, DDR-1010) | 36/36 | rate < 8% |
| `29c792a8b8f3b056` (pre-probe, here) | 20/20 | rate < 14% |

**Not settled: OPEN-2 is not closed, and nothing here says it is fixed.** What
changed is where the remaining evidence lives. The local reproduction route is
**exhausted** — 56 clean runs across the two kernels that matter, including the
exact binary the failure was first seen on. Further local campaigning on this
gate has poor expected value and should not be the next thing anyone runs.

The live evidence is now **CI-side**, and it is not idle:

- DDR-1019 showed that one CI `[apfreeze]` (shard 9) was a **panic symptom** —
  the losing branch of the panic-printer latch — not a scheduler defect, proven
  by disassembling the exact CI binary.
- That means the historical `[apfreeze]` corpus is **at least three different
  producers** wearing one sentinel, and some of what was counted as "the OPEN-2
  rate" was never one defect.
- DDR-1019's instrument (`panic_stage`, first-loser `cpu`/`vec`/`rip`) is armed
  and will name the next one.

**The honest reading of the original "~1 in 4":** it was one session's small
sample, and it has not held up against 56 subsequent runs. It should not be
quoted as a rate again.

## 5. A methodology defect in this campaign — mine, and it would have been vacuous

The runner copied "the capture" after each run by globbing `build/*.log.fail-*`
and `build/blkint.log`. On a **passing** run neither exists, so it fell back to
writing make's stdout. Every one of the 20 files is **3010 bytes, identical size,
with zero `[hb]` lines** — make output, not a serial log.

I then ran `grep -l "apfreeze\|gs FAIL\|NEXUS KERNEL PANIC" run*.log`, got
nothing, and was about to report that as evidence. **It would have been vacuous:**
those strings cannot appear in make output whether or not the kernel emitted
them. The claim in §3 stands only because of the `GLOBAL_FORBIDDEN` + rc=0
argument, which is independent of the captures.

This is the same class this session hit five times inside gates (DDR-1016 §5,
DDR-1017 §4, DDR-1018 §3, DDR-1020 §5 twice) — a check whose only reachable
answer is the passing one — and it is worth recording that it recurs in
**campaign tooling**, not just in probes. **A future campaign must point
`SERIAL_LOG` at a per-run path and assert the file contains boot output before
scanning it.**

## 6. Next

Do **not** re-run this shape. The named next steps for OPEN-2 are CI-side:

1. Watch every CI heartbeat for `panic_stage=`. `stage=1` is DDR-1019's case
   again and narrows the question to `kputs`; `loser_vec` names the second
   exception.
2. On any new `[apfreeze]`, **resolve its RIP against its own binary first** —
   three producers are known and the offsets differ per build.
