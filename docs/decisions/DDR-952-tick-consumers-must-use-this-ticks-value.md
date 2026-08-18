# DDR-952 — the tick consumers re-read `g_ticks`; the atomic increment was only half the fix

Status: ACCEPTED
Extends: DDR-889 / DDR-921 (`g_ticks++` made atomic). Does not supersede them.
Governs: `kernel/idt.c` `timer_tick()`

## a. What DDR-889/921 fixed, and what it left behind

DDR-921 replaced `g_ticks++` with `__atomic_add_fetch(&g_ticks, 1,
__ATOMIC_RELAXED)`. That is correct and stays: it stops two CPUs entering
`timer_tick` concurrently from both reading N and both writing N+1, i.e. it stops
ticks being **lost**.

It did not change the consumers. After the atomic increment, `timer_tick` still
read the **global** three more times:

```c
__atomic_add_fetch(&g_ticks, 1, __ATOMIC_RELAXED);   /* return value DISCARDED */
...
if ((g_ticks % 10u)  == 0) net_poll_tick();          /* re-read */
if ((g_ticks % 100u) == 0) virtio_blk_watchdog();    /* re-read */
if ((g_ticks % 500u) == 0) { kputs("[hb] t="); kputdec(g_ticks); }
```

Each re-read is a fresh load of a value another CPU may have advanced in the
meantime. The increment is atomic; the *decision made from it* is not.

## b. Why this is worse than the duplicate print that motivated DDR-921

Two CPUs incrementing to N and N+1 produce two possible outcomes:

- **Both re-read N.** The heartbeat prints twice. Noisy, harmless, and visible.
- **Both re-read N+1.** The value N is matched by *nobody*. The `% 100` blk
  watchdog scan and the `% 10` lwIP timer pump are **skipped outright** for that
  tick, and nothing in the log says so.

The second outcome is the dangerous one and it is silent. DDR-921's own text
lists exactly this consequence ("can skip the %100 and %10 arms entirely") as
the reason the lost increment mattered — but the fix it applied does not prevent
it, because the skip comes from the consumers' re-read, not from the increment.

## c. The fix

`__atomic_add_fetch` already returns the post-increment value. Take it:

```c
const uint64_t now = __atomic_add_fetch(&g_ticks, 1, __ATOMIC_RELAXED);
```

and use `now` for all three modulo arms and for the heartbeat's printed value.
Every tick value is then handled **exactly once, by exactly one CPU**, by
construction rather than by argument. `g_ticks` remains the shared clock for
every other reader in the tree; only `timer_tick`'s own per-tick decisions
switch to the local value. RELAXED is still sufficient — unchanged reasoning
from DDR-921 §Decision.

## d. Evidence, and its honest weight

Log sweep across `build/*.log` and `build/gatelogs/*.log`:

| log | date | `[hb]` lines | duplicates |
|---|---|---|---|
| `build/shell_serial.log` | 2026-08-17 12:43 | 20 | **1** (`t=3000`) |
| `build/cadence.log` | 2026-08-16 02:13 | 19 | 0 |
| all other gatelogs | — | — | 0 |

The atomic increment landed in `e0ffac0`, **2026-08-15 21:41**. Both logs above
postdate it, and one still shows a duplicate — so the duplicate is not fully
explained by the lost increment DDR-921 fixed.

**That is N=1 and it does NOT confirm the mechanism** (RULE 20: no conclusions
from a single sample). A single duplicated line is also consistent with serial
interleaving. This DDR therefore rests on **code inspection**, which is not
statistical: the consumers demonstrably re-read a shared counter that another
CPU can advance between the increment and the test. That is a defect whether or
not it has yet been caught in a log, and the skip half of it would be invisible
in logs by definition. The N=1 duplicate is recorded as corroboration, not proof.

## e. Gate

No new gate. This changes the correctness of an existing counter's consumers;
its regression surface is the existing timing-sensitive suite, exactly as
DDR-921 §Gate reasoned. Required: warning-clean build, `smoke-blkmq` rc=0, and
the three `ci-*-check` targets.

Build: rc=0, **0 warnings, 0 errors**.
`ci-shard-check` OK (144 gates / 6 shards, 5 excluded), `ci-probe-rodata-check`
OK (55 ELFs), `ci-start-align-check` OK (39 entry points).
`smoke-blkmq` NOT yet run — two CI runs were in flight on 889a059 and §6.0-A
bars local QEMU. It is the first thing to run when they clear.
