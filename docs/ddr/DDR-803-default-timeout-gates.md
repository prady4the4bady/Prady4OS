# DDR-803 — twelve gates inherit an unstated 30 s window they cannot meet

**Status:** diagnosed. **Change proposed, cost quantified — see "The decision
this needs".**
**Date:** 2026-07-29
**Relates to:** DDR-785 (early exit), DDR-788 (window raise), DDR-791
(`GLOBAL_FORBIDDEN`), the standing rule *"do not widen timeouts as a substitute
fix"*.

## What was actually measured

CI runs 30447042919 (`434ff7a`) and 30448425988 (`bf5b4c4`) failed on
`smoke-vfs-bigwrite` and `smoke-smpuser` respectively, both on a **missing
required sentinel**, both passing locally.

My first hypothesis was that the two probes added by DDR-800/801 slowed the
boot. **Measured and refuted** — same config, verified-different kernel SHAs:

| arm | time to `[smp] user on AP OK` | serial |
|---|---|---|
| both probes spawned | **8.3 s** | 11,331 B |
| both probes disabled | **8.4 s** | 11,087 B |

The probes cost nothing. I was about to fix something that was not broken.

The real mechanism, from the job log timestamps:

```
12:07:00  EXTRA_SENTINEL="$(printf 'PRADYOS_BIGWRITE_OK')"    <- gate starts
12:07:30  [smoke] FAIL — required pattern not found           <- exactly 30 s
```

`PRADYOS_BIGWRITE_OK` appears **nowhere** in the serial capture of the whole
job. `boot_test.sh` line 19 is `TIMEOUT_S="${TIMEOUT_S:-30}"`, and
`smoke-vfs-bigwrite` never sets it.

**Twelve gates inherit that 30 s default:** `smoke-sfs-dirs`,
`smoke-sfs-unlink`, `smoke-rootmount`, `smoke-fsrm`, `smoke-sysinfo`,
`smoke-time`, `smoke-dmesg`, `smoke-setname`, `smoke-wxkernel`,
`smoke-sfsroot`, `smoke-vfs-bigwrite`, `smoke-sfs-btree`.

Every one of them also declares a `FORBIDDEN_SENTINEL`, so per DDR-785 they are
**not** eligible for early exit: they run the full window and the sentinel must
land inside it. Their sentinels are emitted late in boot, after SFS
provisioning. Locally the boot reaches a late marker in 8.3 s; a shared TCG
runner is several times slower, which puts 30 s inside the noise band.

That is why the failures wander between gates and vanish locally — the same
signature previously attributed to BUG-1, and why `smoke-setname` failed earlier
this session (run 30405322967). That earlier failure had a *second*, real cause
(my mis-sized `rtcmonotest`, fixed in DDR-796's addendum), which is exactly how
this stayed hidden: a genuine defect masked a systemic one.

## Why this is not "widening a timeout as a substitute fix"

The standing rule exists to stop a bigger window standing in for a real repair.
This is a different situation, and the distinction is worth being explicit about
rather than assuming:

* the feature is **not** broken — these gates pass locally, and passed on CI runs
  `6a00f72` and `a1d1a57`;
* 30 s was never **chosen** for these gates; it is an unstated default inherited
  by omission, and no DDR ever justified it for a late-boot sentinel;
* DDR-788 already set the precedent in this tree, raising eligible gates to
  120 s for exactly this reason. These twelve were missed because they declare
  `FORBIDDEN_SENTINEL` and so were not in DDR-788's eligible set.

If the evidence had shown the sentinel arriving at, say, 25 s and racing, a
window bump would be papering. It never arrives at all in 30 s.

## The decision this needs — the cost is not free

These gates **cannot early-exit**, so a longer window is spent in full on every
run, every time, pass or fail:

| window | cost across 12 gates | delta vs today |
|---|---|---|
| 30 s (today) | 6 min | — |
| 90 s | 18 min | **+12 min per CI run** |
| 120 s (DDR-788 parity) | 24 min | **+18 min per CI run** |

DDR-785 saved 46.5 min/run; this gives 12–18 of it back. That is a real
trade-off, not an implementation detail, which is why it is recorded here rather
than absorbed silently.

**Proposed: 90 s.** Evidence-based rather than symmetric with DDR-788 — a late
sentinel needs ~8 s locally, a shared TCG runner runs several times slower, and
90 s is roughly a 2× margin on the worst observed CI pacing. 120 s buys margin
nobody has evidence of needing, at +6 min more.

## What this does NOT fix

The `smoke-smpuser` failure in run 30448425988 is **not** this: that gate sets
`TIMEOUT_S=180` explicitly and still missed `[smp] user on AP OK`. It belongs to
the OPEN-1 class and must not be attributed here. Naming one mechanism does not
license attributing every red run to it — that error is what produced four
refuted BUG-1 hypotheses.

The durable fix for both is **less boot work**: every gate boots every probe, so
the critical path grows with each one added. That is its own slice, and it is
the thing that would let the window come back down.
