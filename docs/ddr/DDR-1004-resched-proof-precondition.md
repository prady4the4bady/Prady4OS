# DDR-1004 — the rq-3 resched proof asserted a guarantee the kernel does not make

**Status:** FIXED + verified. Supersedes DDR-883's narrowing, which addressed the
same defect with the wrong predicate.

---

## 1. The artefact

CI run 33268386764 (`workflow_dispatch`, shard 8, tip `30a19ce`), gate 1 of 17:

```
[smoke]   [smp] resched FAIL ipis=0 ran=1
make: *** [Makefile:2135: smoke-rqstress] Error 1
shard 8: FAILED at smoke-rqstress after 1 of 17 gates
```

This was the **third** run on that SHA. The `push` and `pull_request` runs were
both green, 0 of 15 jobs failed each. So it is intermittent, and it appeared on
the first dispatched run after `GLOBAL_FORBIDDEN` was restored at `edcdbc2`.

## 2. It is not a scheduler defect. The assertion is wrong.

`sched_unblock` sends the directed wake IPI **conditionally** (`sched.c:1825`):

```c
for (int c = 0; c < PERCPU_MAX; c++) {
    struct percpu *o = percpu_get((uint32_t)c);
    if (c != self && o && o->present && o->idle) {
        smp_resched_one((uint32_t)c);
```

and its own comment states the contract:

> "kick an idle CPU so it steals this thread NOW rather than on its next 10 ms
> tick. Directed IPI to the first idling non-self CPU; **the timer remains the
> backstop if none is (visibly) idle**."

The IPI is best-effort **by design**. The proof nevertheless required
`g_resched_ipis > before` on every boot with `cpu_count > 2`. When no AP happened
to be visibly idle at that instant, zero IPIs were sent, the thread still ran on
the timer backstop — `ran=1`, the property that actually matters — and a correct
system printed FAIL.

## 3. DDR-883 found this and fixed it with the wrong predicate

DDR-883 hit the identical signature (`ipis=0 ran=1`) at `-smp 2`, diagnosed it
correctly — *"a correct system failing its own test"*, deterministic 5/5 — and
guarded the IPI term with `lapic_cpu_count() > 2`.

But the real precondition is **"an idle non-self CPU was visible at unblock"**,
not a CPU count. Those coincide at 2 CPUs and diverge at 4, where all APs can
simply be busy at that moment. So the false failure survived at 4 CPUs, rare
enough to read as a flake. That is what shard 8 hit.

## 4. Rate, measured both ways

| | result |
|---|---|
| CI, tip `30a19ce` | **1 failure in 3 runs** (push green, PR green, dispatch FAIL) |
| local, kernel `60b35c96d70253f5` | **0 failures in 20 runs**, one hash |

20 clean local runs are why this could not be root-caused by reproduction, and
why it had to be read out of the source instead.

## 5. The fix — establish the precondition, and report three outcomes

The settle loop waited 20 ticks *hoping* an AP would go idle and never checked.
It now waits for one and records whether it ever saw one (`idle_seen`), then:

| outcome | meaning |
|---|---|
| `[smp] resched OK` | ran, and the IPI was sent (or not expected at ≤2 CPUs) |
| `[smp] resched SKIP no-idle-ap ran=1` | ran, but no idle AP was ever visible — the rq-3 path was **not exercised** |
| `[smp] resched FAIL ipis=… ran=… idle=…` | a real failure, now with the precondition printed |

**SKIP exists so the fix cannot become a way to always pass.** Reporting OK when
the kick was never owed would silently stop testing the kick — the vacuity trap
DDR-973 §6 and DDR-996 each caught once. SKIP contains neither "OK" nor "FAIL",
so it trips no gate sentinel and no `GLOBAL_FORBIDDEN` entry, and a run of them
is legible in the log as the coverage gap it is.

The FAIL line now prints `idle=`, because `ipis=0 ran=1` alone cannot distinguish
"the kick was owed and missing" from "the kick was never owed".

## 6. Verified — and specifically verified NOT to be a no-op

Kernel `bb9c6187a30bb0dd` (was `60b35c96d70253f5`), warning-clean at `-Werror`.

The load-bearing check is not that the gate passes — a disabled test also passes.
It is that the arm **still exercises the IPI path**:

```
run 1 rc=0  [smp] resched OK
run 2 rc=0  [smp] resched OK
run 3 rc=0  [smp] resched OK
```

Three consecutive `OK`, not `SKIP` — so `idle_seen` is normally true, the IPI is
still required, and it is still observed. The assertion was narrowed to the
kernel's actual contract, not switched off.

Hygiene on the new kernel: `smoke-selftest` 7/7, `smoke-shell`, `smoke-blkmq`,
`smoke-blk-integrity`, `smoke-rqstress-liveness` all PASS; `ci-shard-check` OK
(156 gates / 10 shards / 7 excluded), `ci-probe-rodata-check` OK (61 ELFs).

### 6.1 What is NOT measured

The **SKIP branch has never been observed firing.** It cannot be forced without
holding every AP busy across the unblock, which this probe has no mechanism to
do. It is therefore reasoned-correct but unexercised, and recorded as such rather
than claimed — the same treatment DDR-998 gave its M3 mutant.

And the residual race is stated in the code rather than hidden: `idle_seen` is
sampled just before `sched_unblock`, and a CPU can leave idle in between. The
window is far narrower than the old unconditional assertion but is not zero, so
**a FAIL with `idle=1` is strong evidence, not proof.**
