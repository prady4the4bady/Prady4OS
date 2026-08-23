# DDR-984 — OPEN-14: `[sfs] unlink/rmdir FAIL`, and why it was undiagnosable

Status: OPEN — instrumented, not fixed. No named mechanism yet, so
§NON-NEGOTIABLE 3 forbids a fix.
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§INV.4).

---

## 1. The capture

CI run 32614058580, shard 4, head `0480657` (a **docs-only** commit), gate
`smoke-percpu`:

```text
[sfs] hier dirs OK
[sfs] unlink/rmdir FAIL
make: *** [Makefile:2680: smoke-percpu] Error 1
shard 4: FAILED at smoke-percpu after 2 of 14 gates
```

`smoke-percpu` has nothing to do with SFS. It failed because
`unlink/rmdir FAIL` is in `GLOBAL_FORBIDDEN` and DDR-791 makes a probe failure
redden whichever gate's boot it lands in. That rule worked exactly as intended.

`tools/ci/classify_failure.sh` returned **NEW SIGNATURE** — genuinely a first
occurrence, not a known intermittent wearing a new label.

## 2. Why nothing could be concluded from it

`main.c`'s DDR-741 probe collapsed **twelve** assertions into one `int ok` and
printed a single word:

```c
if (vfs_create(cap, smnt, "/A.TXT", &f) != 0) ok = 0;
… ten more …
kputs(ok ? "[sfs] unlink/rmdir OK\r\n" : "[sfs] unlink/rmdir FAIL\r\n");
```

So the log said *that* it broke and nothing about *which* step or with what
return code. Create? The tombstone re-create? The ENOTEMPTY check? The readdir
scan? All indistinguishable.

**This is the DDR-824 lesson, in a second probe.** DDR-824 recorded it for the
SFS churn probe — *"Printing ONLY the matching lines threw away the diagnosis …
the `op=` line — the one naming WHICH operation broke — contains none of the
forbidden string, so it was never printed"* — and the fix there was to print 40
lines of context. That does not help when the probe never emits a detail line at
all. The lesson generalises: **a probe that reports a boolean is a probe whose
failures cost a CI run and teach nothing.**

## 3. Not reproduced locally

6/6 `smoke-percpu` PASS on the current tip. So it is rare, and there is no
mechanism to name — hence no fix here.

**Not claimed:** that it is or is not a regression from this PR. The commit it
appeared on touches no VFS or SFS code. The one indirect path I can construct is
that `tcb.agent_caps` (DDR-982) grew `struct tcb` and could shift a kheap size
class, disturbing heap layout — but that is a story, not a measurement, and
§NON-NEGOTIABLE 3 exists to stop exactly this kind of plausible-sounding
attribution. Recorded as a hypothesis to test **if** the next capture points at
an allocation-shaped failure, not as a cause.

## 4. What ships: the probe now names the step

Each assertion becomes a numbered step recording the first failure and its `rc`:

```text
[sfs] unlink/rmdir detail step=9 rc=0
[sfs] unlink/rmdir FAIL
```

Properties that matter:

- **Failure path only.** A healthy boot prints the same single OK line as
  before, so no gate parser changes and no extra steady-state output.
- **Detail precedes the summary**, so `GLOBAL_FORBIDDEN`'s 40-line context
  window carries it (DDR-824's mechanism, now given something to carry).
- **Mutation-checked.** Inverting step 9's expectation produced exactly
  `detail step=9 rc=0` — the instrument fires, and names the right step. An
  instrument that never fires is worse than none (DDR-973).

The signature is registered in `classify_failure.sh` as OPEN-14 so the next
session matches it instead of starting over — which is what that script exists
for, and what it told me to do when it returned NEW SIGNATURE.

## 5. Also refined here

`classify_failure.sh`'s `[apfreeze]` entry now reads **"CHECK FOR A PANIC ABOVE
IT FIRST (DDR-979 §3)"**. That detector has a demonstrated false-attribution
mode: it fired downstream of a kernel panic and was first read as a B#3
recurrence. The classifier is where a future session meets that trap, so the
warning belongs there and not only in a DDR.

## 6. Reopen / close conditions

- **Close** when a capture carries `detail step=N rc=R`, the mechanism is named
  from it, and a fix is gated.
- Until then OPEN-14 stays open. It matters for the release: an undiagnosable
  intermittent that can redden *any* of the 149 gates is a standing threat to
  the 3-consecutive-greens rule, and that — not the SFS behaviour itself — is
  why this was worth interrupting the release path for.
