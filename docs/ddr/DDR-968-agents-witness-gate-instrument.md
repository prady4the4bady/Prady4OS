# DDR-968 — `smoke-agents`: instrument the witness gate, do not fix it

Status: ACCEPTED. Written before the code it governs (R16).
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§0.4).
This DDR authorises **instrumentation only**. §6.0-B forbids a fix here and
none is proposed.

## 1. Which line is missing, and what actually emits it

`smoke-agents` requires three sentinels:

```
PRADYOS_AGENTS_OK
AGENT KRYOS active
AGENT SOLIN inactive
```

The failing capture (shard 2, `9231eab`) printed the **first** — twice — and
died on the other two. That split is informative and has not previously been
used: the three sentinels come from **two different blocks** of the compositor's
main loop, and only one of them stalled.

| sentinel | block | fires when |
|---|---|---|
| `PRADYOS_AGENTS_OK` | roster-change block (`compositor.c:928`) | the 8-byte `SYS_AGENT_ROSTER` snapshot differs from the last |
| `AGENT <NAME> active/inactive` ×8 | one-shot metrics witness (`compositor.c:943`) | `m[0].pid != 0 && m[0].dispatches >= 1` |

So the compositor loop was **alive and iterating** — `PRADYOS_AGENTS_OK`
requires a fresh `SYS_AGENT_ROSTER` round trip — while the DDR-914/737 witness
never armed. This rules out "the compositor died" and "the compositor never got
scheduled", which a bare missing-sentinel report leaves open.

## 2. The two-term predicate, and why one term is already implausible

Read the kernel side of the guard:

- **`m[0].pid`** is claimed in `sys_spawn_agent` (`sys_aether.c:198-203`) the
  moment the hook returns `pid >= 0`, and is *retained past exit* by design
  (DDR-735 post-mortem retention, `sys_aether.c:245-248`). The failing log
  contains `aetherd: spawned agent PID=81`, so the claim ran.
- **`m[0].dispatches`** is the scheduler's switch-in count
  (`sched.c:1225`, incremented under the claim), zeroed at spawn
  (`sys_aether.c:203`) and captured authoritatively at exit by
  `agent_metrics_reap`.

By that reading `pid` was non-zero and **`dispatches == 0` is the term that
held the witness down: the agent thread existed and was never switched in.**
That is consistent with the other measured fact in the capture — `preempt`
frozen at 1514 across 23 heartbeats (~11 s) with `rqdepth` pinned at 11.

**This is exactly why no fix ships here.** The paragraph above is an inference
from source plus a partial log; the log never printed either value. §6.0-B
requires the failing run's instrument output, and for this predicate there has
never been any. A confident-looking derivation is precisely the thing this
project has retracted twice before (§0.1, §0.2).

## 3. Decision — print the predicate, bounded

While the witness has not fired, the compositor emits:

```
PRADYOS_AGENT_WITNESS_WAIT pid=<u> disp=<u> state=<u> n=<frames>
```

Constraints it is built to:

- **Bounded output.** At most 24 lines per boot, spaced 128 frames apart. A
  frame loop cannot flood the serial log, and the gate's own `grep -qF` matching
  is unaffected.
- **Near-zero cost on green runs.** The witness normally arms within the first
  frames, after which `metrics_said` skips the block entirely; a passing boot
  emits zero or one of these lines.
- **Prints even when frozen.** Deliberately *not* print-on-change-only: a frozen
  predicate is the signal, and a single line followed by silence is
  indistinguishable from a stopped loop. The rising `n=` is what separates them.
- **No new writable global (DDR-826).** Both counters are locals in the
  compositor's `main` loop, alongside `metrics_said`.
- **No new sentinel collision.** `PRADYOS_AGENT_WITNESS_WAIT` contains no
  substring of any `FORBIDDEN_SENTINEL` in the Makefile (checked against all of
  them, including `AGENT_METRICS FAIL` and `AGENTMEM FAIL`), so it cannot fail a
  gate it merely passes through. It is emitted by every compositor-running gate,
  which is why that check mattered.

## 4. What the next red will say

| capture | reading | belongs with |
|---|---|---|
| `disp=0` with `n` rising | the agent never ran — scheduler did not pick it | DDR-936/947 run-queue work |
| `disp>=1` yet no `AGENT` lines | the witness predicate is wrong, not the scheduler | its own DDR, in the DDR-914/737 line |
| `pid=0` | the roster slot was never claimed despite a successful spawn | `sys_spawn_agent` slot bookkeeping |
| no `WITNESS_WAIT` at all, no `AGENT` lines | the compositor loop stopped | not an agent defect |

The fourth row is the one worth stating explicitly: it is the reading the
current evidence *cannot* rule out and the new line disambiguates for free.

## 5. What this is not

Not a fix, and not a claim about attribution. §"A FIFTH signature" in
`build_status.md` leaves attribution open on the grounds that the same SHA has
been observed passing and failing concurrently, and nothing here changes that.
No local A/B is run for the same reason recorded there: the failure does not
reproduce locally, so both arms would pass and discriminate nothing.

## 6. Verification bar for the instrument itself

The instrument is correct if it does **not** perturb the gates. `smoke-agents`
green locally (it already is, 5/5 previously) plus the §7 hygiene set,
`ci-probe-rodata-check` included. Since a passing run emits at most one of these
lines, a green local run proves only that; the line's value is realised on a CI
red, and closure of the fifth signature waits on that capture.
