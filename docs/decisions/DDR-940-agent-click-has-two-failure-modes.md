= DDR-940 — `smoke-agent-click` has TWO distinct failure modes; DDR-936 describes one

**Status:** ACCEPTED. Measurement + diagnosability. No fix.
**Date:** 2026-08-16
**Evidence:** local repro, tip `3727af7` + the `sys_exit` pid instrument below.
**Lineage:** DDR-936 (mode A) → **DDR-940 (this: mode B is a separate defect)**.
**Invokes §6.0-C** — one DDR per root cause. These must not be merged.

## First: DDR-936's two candidate gates are ELIMINATED

The `ubcas=`/`ubrq=` instrument (`e4c45c0`) read out on a genuinely failing
local run. **All 19 heartbeats of the failing boot read `ubcas=0 ubrq=0`** —
not a drained-counter artifact, checked across every heartbeat, not just the
tail.

Per DDR-936's own decision table, both zero means every `sched_unblock` in the
entire boot enqueued successfully. So:

- the `THREAD_BLOCKED` CAS gate (`sched.c:1223`) — **not it**;
- the `rq_on` test-and-set gate (`sched.c:147`) — **not it**.

DDR-936 narrowed to those two and both are now excluded by measurement. If the
thread is genuinely never running, the defect is in the **pick**, not the
enqueue. That is a different hypothesis space and DDR-936 should be read as
open at that boundary, not as pointing anywhere.

## Second, and more important: the local failure is a DIFFERENT bug

Reproduced `smoke-agent-click` locally: **2 failures in 18 runs (~11%)**.
Neither local failure matches the CI failure DDR-936 was written from.

| | mode A (CI 31926397044) | mode B (local) |
|---|---|---|
| assertion hit | 2nd — "did not run to completion" | **1st — "card click did not trigger the agent"** |
| `PRADYOS_AGENT_TRIGGER` | present, `pid=82` | **absent entirely** |
| `PRADYOS_RIPPLE_OK` | present | **absent** |
| `PRADYOS_MOUSE_OK` | — | **absent** |
| `PRADYOS_AGENTS_OK` | present | present (x3) |
| `[input] virtio pointer up` | present | present |
| reading | agent spawned, never ran | **the press never reached the dispatch** |

In mode B the pointer driver came up and the readiness sentinel fired (so the
injector did inject), but the compositor never took the button-down edge:
`click_ripple(ms.x, ms.y)` is the **unconditional first statement** of the
`if (down)` block, so no `PRADYOS_RIPPLE_OK` proves the dispatch was never
entered. This is exactly the "none of the above" row of DDR-937's table — the
event was dropped or coalesced before the compositor polled it.

**Mode B is not a scheduler defect at all.** It is an input-delivery /
edge-detection defect (`down = ms.buttons && !prev_btn` against a polled
device). Merging it into DDR-936's scheduler investigation would have sent the
whole hunt in the wrong direction, which is precisely what §6.0-C forbids.

## Why this was invisible until now

The gate's two assertions print different messages, but every previous report
of this flake recorded only "smoke-agent-click failed". The two modes were
being counted as one intermittent with a single hit rate, so evidence from one
was used to reason about the other — including by me, in DDR-936.

## The instrument that produced this

`sys_exit`'s log line carried no pid:

```
[user] sys_exit(0) — thread terminating
```

Read after `PRADYOS_AGENT_TRIGGER … pid=82`, a bare `sys_exit(0)` is ambiguous
between "the triggered agent exited before printing anything, so it DID run"
and "an unrelated thread exited, so the agent never ran" — **opposite
conclusions from the same line**. Now:

```
[user] sys_exit(0) pid=81 — thread terminating
```

Also newly visible: exit codes are not all 0. This boot shows
**25 x `exit(0)`, 4 x `exit(127)`, 2 x `exit(42)`**. The `127`s were previously
invisible and are unexplained; recorded here as an observation, not a claim.

## Next steps

1. **Mode B** — instrument the input edge: whether `SYS_SURFACE_POLL`/the mouse
   read ever returns `buttons != 0` in a failing run. If the button state never
   arrives, the defect is below the compositor (virtio-input / event queue); if
   it arrives but `prev_btn` was already set, the edge detector lost it.
2. **Mode A** — needs a fresh capture now that the enqueue gates are excluded.
   Instrument the pick, not the unblock.
3. Track the two modes with **separate hit rates**. The "~20%" figure in the
   backlog is the sum of two unrelated defects and is not a useful number for
   either.
4. `exit(127)` x4 — identify the source before assuming it is benign.

## Verification bar

Diagnosability only. Build warning-clean, `sentinel_collision.sh` OK (159,
unchanged — the pid is a field on an existing line, not a new sentinel),
`smoke-agent-click` run 10x to characterise both modes rather than to pass.
