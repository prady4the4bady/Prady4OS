= DDR-939 — `rqstress FAIL n=0` has two causes; apply DDR-934's spawn check here too

**Status:** ACCEPTED. Diagnosability only — no scheduler change.
**Date:** 2026-08-16
**Evidence:** CI run 31928075088 (tip `5016b29`), `smoke-privacy-netfilter`
reddened by DDR-785's foreign-probe rule on `[smp] rqstress FAIL n=0`.
**Lineage:** DDR-934 (blk probes) → DDR-936 (agent spawn) → **DDR-939 (this)**.

## The observation

```
[smp] rqstress FAIL n=0
```

`n` is `g_rqs_done` — workers that **completed**. `rqstress_proof` launches
**24** kernel threads in 3 waves of 8 and requires all 24. `n=0` means not one
of the 24 finished. The counter exists precisely to make this distinction; its
own comment says a bare FAIL "cannot distinguish '23 landed late' from 'the
runqueues lost a thread'". Zero of 24 is not a marginal race.

## This is the third subsystem with one signature

| gate | symptom | reading |
|---|---|---|
| `smoke-agent-click` | `pid=82`, no `AGENT_START` (DDR-936) | thread created, never ran |
| blk probes | `done=0x0` (DDR-930/934) | workers created, never ran |
| **`rqstress`** | **`n=0`** | **24 workers, none completed** |

Three unrelated subsystems, one shape. That is the DDR-936 argument
strengthened, not a new one.

## The missing-IPI theory is now dead a fourth time, on positive evidence

`rqstress_proof` **already calls `smp_resched_all()`** after each wave
(`kernel/main.c:583-585`). It has the wake IPI and still reports `n=0`.

Previous refutations were arguments (DDR-934: the idle loop is `sti; hlt` and
wakes on its own timer within a tick; DDR-936: `smoke-agent-click` is a
uniprocessor boot with no AP at all). This one is a direct measurement: the
probe that does the waking fails anyway. Do not propose it again.

## But `n=0` still has TWO causes, and we cannot tell them apart

```c
for (int i = 0; i < 8; i++)
    sched_create(rqstress_worker, 0, "rqs");   /* return DISCARDED */
```

`sched_create_state` returns NULL if `kmalloc` fails for either the TCB or the
16 KiB kernel stack. The return is thrown away here, exactly as it was in the
blk probes before DDR-934. So `n=0` is consistent with:

- **(a) scheduling loss** — 24 threads exist and never run (the DDR-936 class);
- **(b) allocation failure** — `sched_create` returned NULL and no thread was
  ever created. 24 workers cost 24 x (TCB + 16 KiB) ~= **384 KiB** of kernel
  heap, requested late in boot after many probes have already allocated. This
  is a much larger ask than the blk probes' 4 workers (~64 KiB), so rqstress is
  the *most* likely place for (b) to bite, not the least.

These point at completely different subsystems. DDR-934 closed this exact gap
for the blk probes and the same fix was never applied here.

## Decision

Count successful spawns and report them, mirroring DDR-934 verbatim:

```
[smp] rqstress FAIL n=<done> spawned=<n>/24
```

Reading it:

- `spawned<24` ⇒ **allocation failure**. The defect is heap exhaustion at this
  point in boot; the fix is in allocation/lifetime, and DDR-936's scheduler
  hunt does not apply to this instance.
- `spawned=24` with `n=0` ⇒ the threads exist and none ran ⇒ genuinely the
  DDR-936 class, and this becomes its cleanest reproducer: 24 threads is a far
  louder signal than one agent, and it already carries the `ubcas=`/`ubrq=`
  counters shipped in `e4c45c0`.

The 6 `probe_thread` spawns immediately above (`main.c:561-562`) discard their
return value too and get the same treatment.

## Why this is worth a slice on its own

If `spawned<24`, then DDR-936's entire framing — "created but never executes" —
is wrong for at least one of its three supporting observations, and possibly
for the agent case too. The unified-signature argument is only as good as the
weakest leg, and this is the leg that has never been checked. Testing it is
cheaper than continuing to build on it.

## Not doing

No scheduler change. No fix for `n=0` itself. §6.0-B: the instrument reports
first. DDR-920/928/932 were each a mechanism named from inference and each was
refuted; DDR-935 shipped on one unrepresentative pass; the "16 gates" claim
earlier in this session was grep output mistaken for evidence. The pattern is
consistent enough to plan around.

## Verification bar

Warning-clean build; `ci-shard-check`; `sentinel_collision.sh` (the FAIL line
gains a field but no new sentinel); `smoke-rqstress-liveness` 3x; plus one gate
that consumes rqstress via DDR-785 (`smoke-privacy-netfilter`) to confirm the
foreign-probe path still reads the line correctly.
