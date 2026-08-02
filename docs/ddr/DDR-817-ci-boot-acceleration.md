# DDR-817 — CI acceleration: shard the boot suite

**Status:** §Design
**Date:** 2026-08-02
**Identified by:** DDR-803 (unclaimed since).
**Sequenced by:** owner decision D-2 — this comes before DDR-820/821 because it
is the only queue item that makes every subsequent item cheaper.

## §Problem, measured rather than assumed

Every feature costs **two consecutive CI greens on the exact tip**. At 2 h 08 m
per run that is ~4 h 15 m of wall-clock per promotion, and the queue ahead of
this DDR is roughly forty items.

Measurement, from run `30741785980` (`main` @ `b823bb5`, green), via the
`.../actions/runs/<id>/jobs` endpoint:

| job | wall-clock |
|---|---|
| **`build-and-boot`** | **2 h 08 m** |
| `arch-bootstrap (aarch64)` | 20 s |
| `arch-bootstrap (riscv64)` | 21 s |
| `aether-layer` | 15 s |
| `code-graph` | 8 s |

**The entire critical path is one serial job.** The other four finish in under
half a minute and are irrelevant to the total. So this is not a "CI is slow"
problem in general — it is one job running 111 QEMU boots back to back.

Inside that job, 119 steps totalling 7 687 s:

| step duration | steps | seconds | share |
|---|---|---|---|
| < 10 s | 22 | 30 | 0 % |
| 10–30 s | 28 | 697 | 9 % |
| 30–60 s | 10 | 322 | 4 % |
| 60–95 s | 26 | 2 104 | 27 % |
| **> 95 s** | **33** | **4 534** | **59 %** |

And the part that matters most for *how* to fix it:

**Building the artefact is nearly free.** checkout 4 s + toolchain install 24 s +
`toolchain-check` 6 s + `musl` 3 s + `lwip` 2 s + `image` 10 s + `mkfs-sfs` 2 s
= **~51 s**. Everything else is QEMU boots.

That ratio — 51 s of build against 7 636 s of boots — is the whole design. Work
that is 99 % embarrassingly-parallel boots and 1 % shared setup should not be
running on one runner.

## §The 180 s tail, and why it is NOT the fix

The nine slowest gates all sit at exactly 180–181 s:

```
smoke-rqstress · smoke-blkmq · smoke-smpuser · smoke-apsched · smoke-msix-ap
smoke-blkverify · smoke-appreempt · smoke-surfdestroy · smoke-rtc-smp
```

All nine are `TIMEOUT_S=180 QEMU_SMP=4` **and all nine declare a
`FORBIDDEN_SENTINEL`**, which under DDR-785 disables early exit. They therefore
burn the full 180 s window *even when they pass* — 1 620 s, 21 % of the job,
spent watching a serial port after the test already succeeded.

It is tempting to reclaim that by extending DDR-785: exit once the required
sentinels appear plus a grace period. **Rejected.** DDR-785 excluded
forbidden-sentinel gates for a real reason — a forbidden pattern can appear
*after* the required ones, and an early exit would not see it. A grace period
weakens that guarantee to "probably long enough", and the only evidence that any
particular grace period suffices is that these gates happen to print their
failures early today. That is an argument about current behaviour, not an
invariant, and S11 says not to trade a real assertion for speed.

**The sharding below reaches the target without touching gate semantics at all**,
so there is no reason to spend the guarantee. The 180 s gates stay 180 s; they
just stop being 180 s *in series with everything else*.

## §Design — a measured, balanced shard matrix

`build-and-boot` becomes a `strategy.matrix` over **6 shards**. Each shard does
the ~51 s of shared setup, then runs only its assigned gates.

Predicted longest shard: `7 636 / 6 + 51` ≈ **22 min**, against a 60 min target
and a 128 min baseline. Six is chosen over four (33 min) and eight (17 min)
because eight puts the 9-gate 180 s cluster under real packing pressure while
six leaves slack, and because concurrent-job limits are a shared resource with
the other four jobs.

### Assignment is by measured duration, not round-robin

Round-robin would be the obvious thing and would be wrong: if the nine 180 s
gates landed unevenly, one shard alone would run 27 min and cap the whole
matrix. The partition is therefore **longest-processing-time-first** over the
measured per-step durations above, committed as a manifest:

```
tools/ci/gate_shards.txt      # <shard>  <make-target>  <measured_secs>
```

The durations are recorded in the manifest so that when a future gate makes a
shard the long pole, the evidence for re-balancing is in the file rather than
being re-derived from the API.

### The failure mode this must not have

A sharded suite's characteristic bug is **a gate that is in no shard**. CI goes
green faster, and the reason it is faster is that it stopped running something.
That is the DDR-791 stale-artefact trap wearing different clothes: a green that
means less than the green it replaced.

So the manifest is checked, in its own fast job:

```
make ci-shard-check
```

which enumerates the `smoke-*` targets in the `Makefile`, subtracts an
**explicit, commented exclusion list** (developer-only gates such as
`smoke-agent-live`, and targets that are not gates), and fails if any remaining
target is unassigned — or assigned twice. The exclusion list is the load-bearing
part: a blanket "every target must be sharded" would be silently satisfiable by
deleting targets, and an unexplained exclusion is exactly how a gate goes
missing. Each exclusion carries its reason on the same line.

### Per-gate attribution is preserved

Today each gate is a named CI step, so a red names the gate. A shard runs its
gates in a loop, which would lose that. The loop therefore wraps each gate in a
`::group::` and, on non-zero exit, prints the failing target name and exits
immediately — so the failing gate is still identified without 111 hand-written
YAML steps, which is itself a maintenance hazard (a gate can be dropped from
`ci.yml` by an ordinary editing mistake and nothing notices).

## §What this does NOT change

- No gate's `TIMEOUT_S`, `EXTRA_SENTINEL`, `FORBIDDEN_SENTINEL`, or
  `GLOBAL_FORBIDDEN` handling.
- No gate's semantics, and no gate removed.
- The two-green promotion rule.

The suite asserts exactly what it asserted before, in the same way. The only
change is which runner each assertion happens on.

## §Gate — `ci-shard-check` plus the run itself

There is no QEMU gate here; the change is CI topology, and a boot test cannot
observe it. What can be asserted:

* **`make ci-shard-check`** — every non-excluded `smoke-*` target is assigned to
  exactly one shard. This is the assertion that protects against the real
  failure mode, and it runs on every push as its own job.
* **The run's own wall-clock** — the matrix must complete in under 60 min, and
  the gate count executed across all shards must equal the count the single job
  ran (111). A shard summary line per job makes the sum checkable.

Stated plainly: the second of these is verified by reading the run, not by an
automated assertion inside it. Building a job that fails on its own wall-clock
would make CI red for reasons unrelated to the tree — a slow runner is not a
defect in the code — so the target is tracked in this DDR and in
`docs/build_status.md` rather than enforced by a timer.

## §Blast radius

`.github/workflows/ci.yml`, a new `tools/ci/gate_shards.txt`, and a new
`ci-shard-check` target in the `Makefile`. No kernel source is touched, so the
kernel SHA is unchanged by this slice — which also means the usual 3-arm A/B has
nothing to vary. The A/B equivalent here is the shard-check itself: remove a
gate from the manifest and `ci-shard-check` must fail.
