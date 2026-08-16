= DDR-928 — make `smoke-agent-click` diagnosable, and record its 55/60 rate margin

**Status:** ACCEPTED. **Diagnosability + a recorded latent risk. No agent change.**
**Date:** 2026-08-16
**Lineage:** DDR-917 / DDR-918 / DDR-920 / DDR-923 (one message, several causes)
and DDR-915 (an agent killed by the ADR-026 rate limiter mid-rendezvous).

## Part 1 — the gate could not be diagnosed at all

`smoke-agent-click` failed in CI 31892607786 with:

```
[aclick] FAIL — the clicked PRAX agent did not run to completion
PRADYOS_AGENT_TRIGGER name=PRAX slot=1 pid=82
```

The gate's failure branch dumped only `tail -20`. `PRADYOS_AGENT_START` is the
**first** statement of the agent's `main()`, so it appears far above a 20-line
tail — meaning its absence from the CI log proves nothing. Every hypothesis
about this gate was therefore untestable from the artifact, which is why it has
sat OPEN across sessions.

The failure branch now prints the agent-relevant lines explicitly
(`PRADYOS_AGENT_*`, `AGENT_RATE_LIMITED`, `AETHER_AGENT_*`, `sys_exit`) plus
`tail -200`. The next failure will say whether the agent started, whether it was
rate-killed, and where it stopped.

This is landed on its own merit and does not depend on the analysis below.

## Part 2 — a latent margin, measured from the code (NOT claimed as the cause)

Counting **counted** syscalls on the `AETHER_TEST_MODE` path
(`user/agent_base.c:136-190`), with `SYS_YIELD` exempt per ADR-036:

| step | counted |
|---|---|
| `printf` AGENT_START | 1 |
| `SYS_SUBMIT_ACTION` | 1 |
| poll loop, worst case | **50** (`SYS_POLL_RESULT` x50) |
| `printf` EXEC/SKIP, `printf` AGENT_DONE | 2 |
| `SYS_EXIT` | 1 |
| **total** | **~55** |

`AETHER_RATE_MAX` is **60** per `AETHER_RATE_WINDOW` (100 PIT ticks = 1 s), and
exceeding it is a **kill** (`sched_exit(137)`), not a stall — the same mechanism
that killed the actiondag agent in DDR-915.

**55 of 60 is an 8% margin.** That is thin enough to be worth recording as a
latent risk in its own right.

Two things keep it from being a standing failure, and both are conditional:

1. The loop breaks early once `SYS_POLL_RESULT` returns anything other than
   `AE_PENDING`. In **sovereign** mode the action auto-approves at submit, so the
   agent spends ~5 counted calls and is nowhere near the budget.
2. Only in **MANUAL** mode does the action stay `AE_PENDING` for all 50
   iterations and drive the total to ~55.

So the exposure depends on the AETHER mode at the instant the clicked agent
submits — and `smoke-agent-click` toggles mode as part of its scenario.

## What is explicitly NOT concluded

That this caused CI 31892607786. There is no `AGENT_RATE_LIMITED` in evidence,
and the gate passes **3/3 locally**, so the failure is not reproducible here.
Per the standing rule, a fix on this basis would be a fix on hypothesis.

`user/agent_base.c` is therefore **unchanged** in this slice.

## The discriminating test, now possible

With Part 1 landed, the next CI failure resolves it directly:

- `AGENT_RATE_LIMITED PID=<n>` present, and no `PRADYOS_AGENT_DONE`
  ⇒ the rate limiter killed it. Fix by pacing the poll loop off the vDSO clock
  exactly as DDR-915 did (bounded polls under the 60/s budget), or by reducing
  the worst-case iteration count.
- `PRADYOS_AGENT_START` absent entirely
  ⇒ the agent never ran; the defect is in spawn/scheduling, not the rate limit.
- `AGENT_START` present, no `AGENT_DONE`, no rate kill
  ⇒ it stalled inside the poll loop; instrument `SYS_POLL_RESULT`'s verdict.

Each branch points somewhere different, which is precisely what the old
`tail -20` made impossible.

## Addendum — `smoke-rtc-smp`'s timeout is NOT the problem

The work queue proposed auditing and likely doubling `smoke-rtc-smp`'s timeout
so the gate would survive long enough for SFS to return its error. Checked, and
that premise does not hold:

```make
smoke-rtc-smp: TIMEOUT_S=180 QEMU_SMP=4
    EXTRA_SENTINEL='PRADYOS_RTC_MONO_OK'
    FORBIDDEN_SENTINEL='RTC_MONO FAIL'
```

The gate's own assertion is `PRADYOS_RTC_MONO_OK` and its budget is already
180 s. It failed on **`btree churn FAIL`**, which is a *foreign* probe — per
DDR-785 any probe reporting FAIL during a boot fails whatever gate is booting.
So the gate did not run out of time; it reached the churn, and the churn
reported an explicit failure.

**No timeout change is warranted, and doubling it would fix nothing.** The gate
is collateral. The actual defect remains `sfs_create` failing at `iter=0`, which
is blocked on a capture naming `-ENOENT` vs `-ENOSPC` (DDR-923) — and per the
standing rule, SFS internals stay untouched until that capture exists.

## RESULT — the widened dump worked, and it REFUTES the rate-margin hypothesis

CI 31911253495 shard 3, with the wider dump in place:

```
PRADYOS_AGENT_START task=test mode=test      <- boot-time agent #1
AETHER_AGENT_EXEC WRITE_FILE /tmp/aether_test.txt
PRADYOS_AGENT_VERIFIED
PRADYOS_AGENT_DONE
PRADYOS_AGENT_START task=test mode=test      <- boot-time agent #2
...
PRADYOS_AGENT_DONE
PRADYOS_AGENT_TRIGGER name=PRAX slot=1 pid=82   <- LAST agent line
AGENT_RATE_LIMITED PID=2742943744
```

Two readings, both now settled:

1. **The clicked agent never printed `PRADYOS_AGENT_START`.** That is the FIRST
   statement of the agent's `main()`. So it never executed a single instruction
   of user code — it cannot have consumed the ~55 counted syscalls the poll loop
   needs. **The 55/60 rate-margin hypothesis recorded above is REFUTED for this
   failure.** The margin remains a real latent risk and the note stands, but it
   is not what this gate is hitting.
2. **The one `AGENT_RATE_LIMITED` is `PID=2742943744`**, the synthetic TCB from
   `aether_sectest`, not pid 82. Reading it as the agent's kill would have been
   the wrong conclusion — exactly the trap the wider dump exists to prevent.

The gate's `awk` is correct: the two `AGENT_DONE` lines precede the TRIGGER, so
they rightly do not satisfy "done AFTER trigger".

## The real question, and it is not local to this gate

The agent is *triggered* (a pid is allocated: `pid=82`) and then never runs.
That is the same shape as DDR-930's `done=0x0`: threads that are created but
produce nothing. See DDR-930 for the convergence — this is now two independent
gates pointing at newly spawned threads not being scheduled.

`user/agent_base.c` remains UNCHANGED, now for a stronger reason than "no
evidence": the evidence positively excludes it.
