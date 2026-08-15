= DDR-900 — `sched_create` does not wake idle CPUs; the probes that compensate pass

**Status:** ACCEPTED (diagnosis). **Fix NOT implemented** — reached at the §5c
context threshold; an unverified scheduler change must not be left in the tree.
**Date:** 2026-08-16
**Lineage:** DDR-886 → DDR-898 (done=0x0) + DDR-896 (agent never started) → **DDR-900**.

## The correlation

| probe | `smp_resched_all()` after spawning? | observed in CI |
|---|---|---|
| `rqstress_proof` (main.c:584-585) | **YES** | passes |
| `blkmq_proof` (main.c:633-635) | **no** | `multi-inflight FAIL done=0x0` |
| `smp_blk_integrity` (main.c:710-714) | **no** | `blk integrity FAIL done=0x0` |

The two probes that omit the wake are exactly the two that report `done=0x0` —
i.e. not one spawned worker ever returned (DDR-898 established that the final
`__atomic_or_fetch` is unconditional, so a returning worker always sets a bit).

## The mechanism

`smp_resched_all` has **three call sites in the whole tree**, all of them probes
in `main.c` (563, 585, 825). It is called from **neither `sched_create` nor
`sched_unblock`**.

So creating a runnable thread does not, by itself, wake an idle CPU. Pickup
depends on:

1. the lockless `rq_has_ready()` hint in the idle loop (`sched.c:349`), whose own
   comment concedes *"a stale read is harmless: a false negative is caught by the
   timer tick"*; and
2. that CPU's next timer tick.

A false negative therefore costs up to a full tick — and the creating thread
meanwhile sits in `while (…) yield()`, which reschedules only locally.

`rqstress_proof` papers over this with an explicit `smp_resched_all()`. The two
blk probes do not, and they are the ones failing.

## Why this is a probe-level fix, not a scheduler change (initially)

The safe, minimal step is to make the two blk probes match the known-good
`rqstress_proof` pattern — one line each, no scheduler semantics touched, and
directly testable against the `prog=` instrument added in DDR-898:

- if `prog=0,0,0,0` disappears once the IPI is added, the wake was the cause;
- if it persists, the wake is not the cause and the scheduler is next.

Whether `sched_create` itself should issue the wake is the **deeper** question.
Doing it there would fix every future caller rather than three hand-patched
probes, but it changes a hot path used by every thread creation in the kernel
and needs its own measurement — it is not a free win, and it is explicitly out
of scope here.

## Does this explain the agent too?

Unclear, and NOT claimed. `smoke-agent-click`'s clicked agent gets a pid and
never prints `AGENT_START` (DDR-896), which is the same "created but never ran"
shape. The agent spawn path was not traced to a `sched_create`/`sched_unblock`
call in this slice, so the connection is a hypothesis only.

## Next actions, in order

1. Add `smp_resched_all()` after the spawn loops in `blkmq_proof` and
   `smp_blk_integrity`. Build, hygiene, run both gates.
2. Read `prog=` on the next failure (DDR-898) to confirm or refute.
3. Only then consider whether `sched_create` should wake, with measurement.
